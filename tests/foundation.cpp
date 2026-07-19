#include "xmin/server/atom_table.hpp"
#include "xmin/server/checked.hpp"
#include "xmin/server/color.hpp"
#include "xmin/server/connection.hpp"
#include "xmin/server/generated/core_protocol.hpp"
#include "xmin/server/property_data.hpp"
#include "xmin/server/resource_registry.hpp"
#include "xmin/server/result.hpp"
#include "xmin/server/server_state.hpp"
#include "xmin/server/surface.hpp"
#include "xmin/server/unique_fd.hpp"
#include "xmin/server/wire.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace {

class ManualClock final : public xmin::server::Clock {
public:
    [[nodiscard]] time_point now() const noexcept override { return now_; }

    void advance(std::chrono::milliseconds elapsed) noexcept
    {
        now_ += elapsed;
    }

private:
    time_point now_{};
};

bool
expect(bool condition, const char *message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

bool
test_checked_arithmetic()
{
    using xmin::server::checked_add;
    using xmin::server::checked_multiply;
    using xmin::server::checked_subtract;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    constexpr auto signed_maximum =
        std::numeric_limits<std::int64_t>::max();
    constexpr auto signed_minimum =
        std::numeric_limits<std::int64_t>::min();
    return expect(checked_add(std::size_t{2}, std::size_t{3}) == 5,
                  "checked_add rejected a valid sum") &&
        expect(!checked_add(maximum, std::size_t{1}),
               "checked_add accepted overflow") &&
        expect(checked_multiply(std::size_t{6}, std::size_t{7}) == 42,
               "checked_multiply rejected a valid product") &&
        expect(!checked_multiply(maximum, std::size_t{2}),
               "checked_multiply accepted overflow") &&
        expect(checked_add(std::int64_t{-9}, std::int64_t{4}) == -5,
               "signed checked_add rejected a valid sum") &&
        expect(!checked_add(signed_maximum, std::int64_t{1}) &&
                   !checked_add(signed_minimum, std::int64_t{-1}),
               "signed checked_add accepted overflow") &&
        expect(checked_subtract(std::int64_t{-9}, std::int64_t{-4}) == -5,
               "signed checked_subtract rejected a valid difference") &&
        expect(!checked_subtract(signed_minimum, std::int64_t{1}) &&
                   !checked_subtract(signed_maximum, std::int64_t{-1}),
               "signed checked_subtract accepted overflow");
}

bool
test_generated_core_protocol()
{
    using xmin::server::CoreOpcode;
    const auto intern = static_cast<std::size_t>(CoreOpcode::InternAtom);
    return expect(xmin::server::core_request_table.size() == 128,
                  "generated core opcode table has the wrong size") &&
        expect(xmin::server::core_request_table[intern].name == "InternAtom",
               "generated core opcode name is wrong") &&
        expect(xmin::server::core_request_table[120].name == "Reserved" &&
                   !xmin::server::core_request_table[120].defined,
               "generated reserved opcode metadata is wrong");
}

bool
test_wire_order(xmin::server::ByteOrder order)
{
    xmin::server::WireWriter writer(order);
    writer.u8(0x12);
    writer.u16(0x3456);
    writer.u32(0x789abcde);
    writer.u64(0x0123456789abcdefULL);
    writer.pad_to_four();

    xmin::server::WireReader reader(writer.data(), order);
    const auto byte = reader.u8();
    const auto word = reader.u16();
    const auto dword = reader.u32();
    const auto qword = reader.u64();
    return expect(byte == 0x12, "wire u8 round trip failed") &&
        expect(word == 0x3456, "wire u16 round trip failed") &&
        expect(dword == 0x789abcde, "wire u32 round trip failed") &&
        expect(qword == 0x0123456789abcdefULL,
               "wire u64 round trip failed") &&
        expect(reader.skip(reader.remaining()), "wire padding skip failed") &&
        expect(!reader.u8(), "wire reader crossed its bound");
}

bool
test_property_byte_order()
{
    constexpr std::array<std::uint8_t, 4> big_endian{
        0x12, 0x34, 0x56, 0x78};
    constexpr std::array<std::uint8_t, 4> little_endian{
        0x78, 0x56, 0x34, 0x12};
    const auto canonical = xmin::server::canonical_property_data(
        big_endian.data(), big_endian.size(), 32,
        xmin::server::ByteOrder::big);
    if (!expect(canonical &&
                    std::equal(canonical->begin(), canonical->end(),
                               little_endian.begin()),
                "property data was not canonicalized")) {
        return false;
    }
    return expect(
               xmin::server::wire_property_data(
                   canonical->data(), canonical->size(), 32,
                   xmin::server::ByteOrder::big) ==
                   std::vector<std::uint8_t>(big_endian.begin(),
                                             big_endian.end()),
               "big-endian property encoding failed") &&
        expect(xmin::server::wire_property_data(
                   canonical->data(), canonical->size(), 32,
                   xmin::server::ByteOrder::little) ==
                   std::vector<std::uint8_t>(little_endian.begin(),
                                             little_endian.end()),
               "little-endian property encoding failed");
}

bool
test_atoms_and_resources()
{
    xmin::server::AtomTable atoms;
    if (!expect(atoms.size() == 68, "predefined atom count is wrong") ||
        !expect(atoms.intern("PRIMARY", true) == 1,
                "predefined PRIMARY atom id is wrong") ||
        !expect(atoms.intern("DOES_NOT_EXIST", true) == 0,
                "only-if-exists created an atom")) {
        return false;
    }
    const auto custom = atoms.intern("XMIN_TEST");
    if (!expect(custom == 69, "custom atom id is wrong") ||
        !expect(atoms.intern("XMIN_TEST") == custom,
                "atom interning is not stable") ||
        !expect(atoms.name(custom) == std::string("XMIN_TEST"),
                "atom reverse lookup failed")) {
        return false;
    }

    xmin::server::ResourceRegistry resources;
    return expect(resources.insert(1, xmin::server::ResourceKind::window, 0),
                  "resource insertion failed") &&
        expect(!resources.insert(1, xmin::server::ResourceKind::pixmap, 2),
               "duplicate resource insertion succeeded") &&
        expect(resources.insert(20, xmin::server::ResourceKind::pixmap, 7),
               "owned resource insertion failed") &&
        expect(resources.is(1, xmin::server::ResourceKind::window),
               "typed resource lookup failed") &&
        expect(resources.erase_owner(7) == 1,
               "client resource cleanup count is wrong") &&
        expect(resources.size() == 1, "server-owned resource was removed");
}

bool
test_unique_fd()
{
    int descriptors[2];
    if (::pipe(descriptors) != 0) {
        std::cerr << "pipe failed\n";
        return false;
    }
    const int owned_number = descriptors[0];
    {
        xmin::server::UniqueFd first(descriptors[0]);
        xmin::server::UniqueFd second(std::move(first));
        if (!expect(!first && second.get() == owned_number,
                    "UniqueFd move did not transfer ownership")) {
            ::close(descriptors[1]);
            return false;
        }
    }
    errno = 0;
    const bool closed = ::fcntl(owned_number, F_GETFD) == -1 && errno == EBADF;
    ::close(descriptors[1]);
    return expect(closed, "UniqueFd did not close its descriptor");
}

bool
write_exact(int descriptor, const std::uint8_t *bytes, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = ::write(descriptor, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool
read_exact(int descriptor, std::uint8_t *bytes, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = ::read(descriptor, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool
test_connection_event_reply_order()
{
    constexpr std::uint32_t owner = 0x00200000;
    int descriptors[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
        std::cerr << "connection-order socketpair failed\n";
        return false;
    }
    xmin::server::UniqueFd peer(descriptors[1]);
    xmin::server::ServerState server(64, 48);
    xmin::server::ServerConfig config;
    config.width = 64;
    config.height = 48;
    config.resource_base = owner;
    config.allow_unauthenticated = true;
    xmin::server::Connection connection{
        xmin::server::UniqueFd(descriptors[0]), std::move(config), server};
    if (!expect(static_cast<bool>(connection.prepare()),
                "connection-order prepare failed")) {
        return false;
    }

    std::array<std::uint8_t, 12> setup{};
    setup[0] = static_cast<std::uint8_t>('l');
    setup[2] = 11;
    if (!expect(write_exact(peer.get(), setup.data(), setup.size()) &&
                    connection.on_readable() && connection.on_writable(),
                "connection-order setup failed")) {
        return false;
    }
    std::array<std::uint8_t, 8> setup_prefix{};
    if (!expect(read_exact(
                    peer.get(), setup_prefix.data(), setup_prefix.size()) &&
                    setup_prefix[0] == 1,
                "connection-order setup reply failed")) {
        return false;
    }
    const std::size_t setup_size = static_cast<std::size_t>(
        setup_prefix[6] | (static_cast<std::uint16_t>(setup_prefix[7]) << 8)) *
        4U;
    std::vector<std::uint8_t> setup_payload(setup_size);
    if (!expect(read_exact(
                    peer.get(), setup_payload.data(), setup_payload.size()),
                "connection-order setup payload failed")) {
        return false;
    }

    // Model a backpressured connection: an asynchronous event is pending
    // before a newer request is dispatched.  Its wire record must precede
    // the reply, even when on_readable runs before on_writable.
    if (!expect(server.broadcast_mapping_notify(1, 8, 1),
                "connection-order event setup failed")) {
        return false;
    }
    const std::array<std::uint8_t, 4> get_input_focus{43, 0, 1, 0};
    if (!expect(write_exact(peer.get(), get_input_focus.data(),
                            get_input_focus.size()) &&
                    connection.on_readable() && connection.on_writable(),
                "connection-order request dispatch failed")) {
        return false;
    }
    std::array<std::uint8_t, 64> output{};
    return expect(read_exact(peer.get(), output.data(), output.size()),
                  "connection-order output read failed") &&
        expect(output[0] == 34 && output[32] == 1,
               "an event was written after a newer reply") &&
        expect(output[2] == 0 && output[3] == 0 &&
                   output[34] == 1 && output[35] == 0,
               "connection-order response sequences are wrong");
}

bool
test_shared_server_state()
{
    constexpr std::uint32_t first_owner = 0x00200000;
    constexpr std::uint32_t second_owner = 0x00400000;
    xmin::server::ServerState server(320, 240);
    if (!expect(server.window(xmin::server::root_window_id) != nullptr,
                "server root window is missing") ||
        !expect(server.window(xmin::server::root_window_id)
                        ->background_pixel == 0x0020252bU &&
                    server.readable_surface(xmin::server::root_window_id)
                        ->pixel(0, 0) == 0x0020252bU,
                "server root did not initialize with the desktop background") ||
        !expect(server.input().pointer_x == 160 &&
                    server.input().pointer_y == 120 &&
                    server.input().modifier_button_mask == 0 &&
                    server.input().pressed_buttons.none(),
                "input snapshot did not initialize at screen center") ||
        !expect(server.input().pressed_keys ==
                    std::array<std::uint8_t, 32>{},
                "input snapshot initialized with pressed keys") ||
        !expect(server.input().keymap_width ==
                    xmin::server::keysyms_per_keycode &&
                    server.input().keymap[
                        96 * server.input().keymap_width] == 0x0000ffc9U &&
                    server.input().modifier_map ==
                        std::vector<std::uint8_t>(
                            xmin::server::core_modifier_map.begin(),
                            xmin::server::core_modifier_map.end()) &&
                    server.input().auto_repeats ==
                        xmin::server::default_auto_repeats &&
                    server.input().pointer_map ==
                        xmin::server::default_pointer_map,
                "generated core input defaults were not installed") ||
        !expect(server.valid_client_resource(first_owner, first_owner),
                "valid first client XID was rejected") ||
        !expect(!server.valid_client_resource(second_owner, first_owner),
                "foreign client XID range was accepted")) {
        return false;
    }

    if (!expect(server.register_client(first_owner) &&
                    server.register_client(second_owner),
                "client event registration failed")) {
        return false;
    }
    server.note_client_sequence(first_owner, 17);
    server.note_client_sequence(second_owner, 23);
    if (!expect(server.broadcast_mapping_notify(1, 96, 1),
                "mapping notification broadcast failed")) {
        return false;
    }
    const auto *first_mapping = server.next_event(first_owner);
    const auto *second_mapping = server.next_event(second_owner);
    const auto *first_mapping_value = first_mapping == nullptr
        ? nullptr
        : std::get_if<xmin::server::MappingNotifyEvent>(first_mapping);
    const auto *second_mapping_value = second_mapping == nullptr
        ? nullptr
        : std::get_if<xmin::server::MappingNotifyEvent>(second_mapping);
    if (!expect(first_mapping_value != nullptr &&
                    first_mapping_value->sequence == 17 &&
                    first_mapping_value->request == 1 &&
                    first_mapping_value->first_keycode == 96 &&
                    first_mapping_value->count == 1 &&
                    second_mapping_value != nullptr &&
                    second_mapping_value->sequence == 23,
                "mapping broadcast lost client-specific event state")) {
        return false;
    }
    server.pop_event(first_owner);
    server.pop_event(second_owner);

    xmin::server::WindowRecord parent;
    parent.id = first_owner;
    parent.parent = xmin::server::root_window_id;
    parent.width = 20;
    parent.height = 10;
    xmin::server::WindowRecord child;
    child.id = second_owner;
    child.parent = first_owner;
    child.width = 5;
    child.height = 4;
    if (!expect(server.add_window(std::move(parent), first_owner),
                "shared parent insertion failed") ||
        !expect(server.add_window(std::move(child), second_owner),
                "cross-client child insertion failed") ||
        !expect(server.window(xmin::server::root_window_id)->children.size() == 1,
                "root child relationship is wrong")) {
        return false;
    }

    auto *stored_parent = server.window(first_owner);
    auto *stored_child = server.window(second_owner);
    static_cast<void>(server.set_window_mapped(*stored_parent, true));
    static_cast<void>(server.set_window_mapped(*stored_child, true));
    server.advance_time();
    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window, second_owner, 2, 0) ==
                    xmin::server::FocusUpdate::updated,
                "window focus update failed") ||
        !expect(server.set_input_focus(
                    xmin::server::FocusKind::none, 0, 0, 1) ==
                    xmin::server::FocusUpdate::ignored,
                "stale focus timestamp was accepted")) {
        return false;
    }
    static_cast<void>(server.set_window_mapped(*stored_child, false));
    if (!expect(server.input().focus.kind == xmin::server::FocusKind::window &&
                    server.input().focus.window == first_owner &&
                    server.input().focus.revert_to == 0,
                "parent focus reversion failed")) {
        return false;
    }
    static_cast<void>(server.set_window_mapped(*stored_child, true));
    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window, second_owner, 1, 0) ==
                    xmin::server::FocusUpdate::updated,
                "pointer-root reversion setup failed")) {
        return false;
    }

    const auto selection = server.atoms().intern("XMIN_SELECTION");
    server.advance_time();
    if (!expect(server.set_selection_owner(
                    selection, first_owner, first_owner, 0) ==
                    xmin::server::SelectionUpdate::updated,
                "initial selection ownership failed") ||
        !expect(server.selection_owner(selection) == first_owner,
                "selection owner lookup failed")) {
        return false;
    }

    xmin::server::ClientMessageEvent message;
    message.format = 32;
    message.window = first_owner;
    message.type = selection;
    message.data[0] = 0x584d494eU;
    if (!expect(server.deliver_client_message(
                    first_owner, 0, false, message) ==
                    xmin::server::EventDelivery::delivered,
                "creator-targeted client event was not delivered")) {
        return false;
    }
    const auto *queued = server.next_event(first_owner);
    const auto *queued_message = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::ClientMessageEvent>(queued);
    if (!expect(queued_message != nullptr &&
                    queued_message->data[0] == 0x584d494eU &&
                    queued_message->sequence == 17,
                "queued client event lost semantic data")) {
        return false;
    }
    server.pop_event(first_owner);

    server.advance_time();
    if (!expect(server.set_selection_owner(
                    selection, second_owner, second_owner, 0) ==
                    xmin::server::SelectionUpdate::updated,
                "selection ownership transfer failed")) {
        return false;
    }
    const auto *selection_event = server.next_event(first_owner);
    if (!expect(selection_event != nullptr &&
                    std::holds_alternative<xmin::server::SelectionClearEvent>(
                        *selection_event),
                "selection transfer did not notify the previous owner")) {
        return false;
    }
    server.pop_event(first_owner);
    server.window(xmin::server::root_window_id)
        ->event_masks.emplace(first_owner, 1);
    server.grab_server(first_owner);
    xmin::server::ActiveGrab active_grab;
    active_grab.owner = first_owner;
    active_grab.window = xmin::server::root_window_id;
    server.input().pointer_grab = active_grab;
    server.input().keyboard_grab = active_grab;
    if (!expect(server.server_grab_owner() == first_owner,
                "server grab owner was not recorded")) {
        return false;
    }
    server.disconnect_client(first_owner);
    return expect(server.window(first_owner) == nullptr,
                  "owner disconnect retained its window") &&
        expect(server.window(second_owner) == nullptr,
               "parent teardown retained a foreign child") &&
        expect(server.window(xmin::server::root_window_id)->children.empty(),
               "root retained a destroyed child") &&
        expect(server.selection_owner(selection) == 0,
               "destroyed selection window remained the owner") &&
        expect(server.window(xmin::server::root_window_id)
                       ->event_masks.count(first_owner) == 0,
               "disconnect retained an event selection") &&
        expect(server.server_grab_owner() == 0,
               "disconnect retained a server grab") &&
        expect(server.input().focus.kind == xmin::server::FocusKind::pointer_root,
               "destroyed focus window did not revert to pointer root") &&
        expect(!server.input().pointer_grab &&
                   !server.input().keyboard_grab,
               "disconnect retained an active input grab");
}

bool
test_property_notifications()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t window_id = owner + 1;
    constexpr std::uint32_t property_change_mask = 1U << 22;
    constexpr std::uint16_t sequence = 73;
    xmin::server::ServerState server(32, 24);

    if (!expect(server.register_client(owner),
                "property-event client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, sequence);
    auto *root = server.window(xmin::server::root_window_id);
    root->event_masks.emplace(owner, property_change_mask);

    xmin::server::WindowRecord target;
    target.id = window_id;
    target.parent = xmin::server::root_window_id;
    target.width = 1;
    target.height = 1;
    if (!expect(server.add_window(std::move(target), owner),
                "property-event target insertion failed")) {
        return false;
    }

    const auto property = server.atoms().intern("XMIN_PROPERTY_NOTIFY");
    const auto type = server.atoms().intern("INTEGER");
    server.advance_time();
    xmin::server::PropertyValue initial{type, 8, {0x11}};
    if (!expect(server.set_property(*root, property, std::move(initial)) ==
                    xmin::server::EventDelivery::delivered,
                "property change notification was not delivered")) {
        return false;
    }
    const auto *queued = server.next_event(owner);
    const auto *changed = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::PropertyNotifyEvent>(queued);
    if (!expect(changed != nullptr && changed->state == 0 &&
                    changed->window == xmin::server::root_window_id &&
                    changed->atom == property && changed->time == 2 &&
                    changed->sequence == sequence,
                "property change notification is malformed")) {
        return false;
    }
    server.pop_event(owner);

    xmin::server::ClientMessageEvent message;
    message.window = window_id;
    for (std::size_t count = 0;
         count < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(
                        window_id, 0, false, message) ==
                        xmin::server::EventDelivery::delivered,
                    "property queue-pressure setup failed")) {
            return false;
        }
    }

    xmin::server::PropertyValue replacement{type, 8, {0x22}};
    if (!expect(server.set_property(
                    *root, property, std::move(replacement)) ==
                    xmin::server::EventDelivery::queue_full &&
                    root->properties.at(property).data ==
                        std::vector<std::uint8_t>{0x11},
                "queue-full property replacement was not atomic") ||
        !expect(server.delete_property(*root, property) ==
                    xmin::server::EventDelivery::queue_full &&
                    root->properties.count(property) == 1,
                "queue-full property deletion was not atomic")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);

    if (!expect(server.delete_property(*root, property) ==
                    xmin::server::EventDelivery::delivered,
                "property deletion notification was not delivered")) {
        return false;
    }
    queued = server.next_event(owner);
    const auto *deleted = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::PropertyNotifyEvent>(queued);
    return expect(deleted != nullptr && deleted->state == 1 &&
                      deleted->window == xmin::server::root_window_id &&
                      deleted->atom == property && deleted->time == 2 &&
                      deleted->sequence == sequence,
                  "property deletion notification is malformed") &&
        expect(root->properties.count(property) == 0,
               "property deletion retained its value");
}

bool
test_passive_grabs()
{
    constexpr std::uint32_t first_owner = 0x00200000;
    constexpr std::uint32_t second_owner = 0x00400000;
    constexpr std::uint8_t key = 38;
    constexpr std::uint8_t other_key = 39;
    constexpr std::uint16_t shift = 1;
    xmin::server::ServerState server(32, 24);

    xmin::server::PassiveGrab wildcard;
    wildcard.kind = xmin::server::PassiveGrabKind::key;
    wildcard.details = xmin::server::passive_grab_details(wildcard.kind, 0);
    wildcard.modifiers =
        xmin::server::passive_grab_modifiers(xmin::server::any_modifier);
    wildcard.owner = first_owner;
    wildcard.window = xmin::server::root_window_id;
    if (!expect(wildcard.details.count() == 248 &&
                    wildcard.modifiers.count() == 256,
                "passive wildcard domains have the wrong bounds") ||
        !expect(server.add_passive_grab(wildcard) ==
                    xmin::server::PassiveGrabUpdate::updated,
                "passive wildcard grab insertion failed")) {
        return false;
    }

    xmin::server::PassiveGrab exact = wildcard;
    exact.details = xmin::server::passive_grab_details(exact.kind, key);
    exact.modifiers = xmin::server::passive_grab_modifiers(shift);
    exact.owner = second_owner;
    if (!expect(server.add_passive_grab(exact) ==
                    xmin::server::PassiveGrabUpdate::access_denied,
                "overlapping cross-client passive grab was accepted") ||
        !expect(server.remove_passive_grab(
                    xmin::server::PassiveGrabKind::key, first_owner,
                    xmin::server::root_window_id, exact.details,
                    exact.modifiers) ==
                    xmin::server::PassiveGrabUpdate::updated,
                "exact passive wildcard subtraction failed") ||
        !expect(server.passive_grabs().size() == 2,
                "wildcard subtraction did not produce two rectangles") ||
        !expect(server.add_passive_grab(exact) ==
                    xmin::server::PassiveGrabUpdate::updated,
                "subtracted passive combination remained reserved")) {
        return false;
    }

    exact.details = xmin::server::passive_grab_details(exact.kind, other_key);
    if (!expect(server.add_passive_grab(exact) ==
                    xmin::server::PassiveGrabUpdate::access_denied,
                "wildcard remainder lost cross-client exclusion")) {
        return false;
    }

    server.disconnect_client(first_owner);
    if (!expect(server.passive_grabs().size() == 1 &&
                    server.passive_grabs().front().owner == second_owner,
                "disconnect retained an owned passive grab")) {
        return false;
    }
    server.disconnect_client(second_owner);
    if (!expect(server.passive_grabs().empty(),
                "passive grab cleanup left stale state") ||
        !expect(xmin::server::passive_grab_modifiers(0x0100).none(),
                "invalid passive modifier domain was materialized")) {
        return false;
    }

    xmin::server::WindowRecord confine;
    confine.id = first_owner;
    confine.parent = xmin::server::root_window_id;
    confine.width = 4;
    confine.height = 4;
    xmin::server::PassiveGrab confined;
    confined.kind = xmin::server::PassiveGrabKind::button;
    confined.details = xmin::server::passive_grab_details(confined.kind, 1);
    confined.modifiers = xmin::server::passive_grab_modifiers(0);
    confined.owner = first_owner;
    confined.window = xmin::server::root_window_id;
    confined.confine_to = first_owner;
    if (!expect(server.add_window(std::move(confine), first_owner),
                "passive-grab confine window insertion failed") ||
        !expect(server.add_passive_grab(std::move(confined)) ==
                    xmin::server::PassiveGrabUpdate::updated,
                "confined passive grab insertion failed")) {
        return false;
    }
    static_cast<void>(server.destroy_window(first_owner));
    return expect(server.passive_grabs().empty(),
                  "destroyed confine window retained a passive grab");
}

bool
test_input_routing()
{
    constexpr std::uint32_t first_owner = 0x00200000;
    constexpr std::uint32_t second_owner = 0x00400000;
    constexpr std::uint32_t parent_id = first_owner | 1U;
    constexpr std::uint32_t child_id = first_owner | 2U;
    constexpr std::uint32_t key_press_mask = 1U << 0;
    constexpr std::uint32_t key_release_mask = 1U << 1;
    constexpr std::uint32_t button_press_mask = 1U << 2;
    constexpr std::uint32_t button_release_mask = 1U << 3;
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(first_owner) &&
                    server.register_client(second_owner),
                "input-routing client registration failed")) {
        return false;
    }
    server.note_client_sequence(first_owner, 31);
    server.note_client_sequence(second_owner, 47);

    xmin::server::WindowRecord parent;
    parent.id = parent_id;
    parent.parent = xmin::server::root_window_id;
    parent.x = 10;
    parent.y = 8;
    parent.width = 60;
    parent.height = 50;
    xmin::server::WindowRecord child;
    child.id = child_id;
    child.parent = parent_id;
    child.x = 5;
    child.y = 6;
    child.width = 20;
    child.height = 15;
    if (!expect(server.add_window(std::move(parent), first_owner) &&
                    server.add_window(std::move(child), first_owner),
                "input-routing window insertion failed")) {
        return false;
    }
    auto *stored_parent = server.window(parent_id);
    auto *stored_child = server.window(child_id);
    static_cast<void>(server.set_window_mapped(*stored_parent, true));
    static_cast<void>(server.set_window_mapped(*stored_child, true));
    stored_parent->event_masks.emplace(
        second_owner, key_press_mask | key_release_mask);
    if (!expect(server.inject_input(6, 0, 18, 17) ==
                    xmin::server::EventDelivery::no_recipient &&
                    server.input().pointer_x == 18 &&
                    server.input().pointer_y == 17,
                "unselected motion did not update pointer state") ||
        !expect(server.set_input_focus(
                    xmin::server::FocusKind::window, parent_id, 0, 0) ==
                    xmin::server::FocusUpdate::updated,
                "input-routing focus setup failed") ||
        !expect(server.inject_input(2, 38, 18, 17) ==
                    xmin::server::EventDelivery::delivered,
                "key press did not propagate to an ancestor")) {
        return false;
    }
    const auto *queued = server.next_event(second_owner);
    const auto *key = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CoreInputEvent>(queued);
    if (!expect(key != nullptr && key->type == 2 && key->detail == 38 &&
                    key->root == xmin::server::root_window_id &&
                    key->event == parent_id && key->child == child_id &&
                    key->root_x == 18 && key->root_y == 17 &&
                    key->event_x == 8 && key->event_y == 9 &&
                    key->state == 0 && key->same_screen &&
                    key->sequence == 47,
                "propagated key event lost routing metadata")) {
        return false;
    }
    server.pop_event(second_owner);

    stored_child->do_not_propagate_mask = key_release_mask;
    if (!expect(server.inject_input(3, 38, 18, 17) ==
                    xmin::server::EventDelivery::no_recipient &&
                    !server.has_pending_event(second_owner) &&
                    (server.input().pressed_keys[38 >> 3] &
                     (1U << (38 & 7U))) == 0,
                "do-not-propagate did not stop a key release")) {
        return false;
    }
    stored_child->do_not_propagate_mask = 0;

    if (!expect(server.inject_input(6, 0, 90, 70) ==
                    xmin::server::EventDelivery::no_recipient &&
                    server.inject_input(2, 41, 90, 70) ==
                    xmin::server::EventDelivery::delivered,
                "focused key event outside the focus subtree was lost")) {
        return false;
    }
    queued = server.next_event(second_owner);
    key = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CoreInputEvent>(queued);
    if (!expect(key != nullptr && key->event == parent_id &&
                    key->child == 0 && key->root_x == 90 &&
                    key->root_y == 70 && key->event_x == 80 &&
                    key->event_y == 62,
                "focused key event used the dispatch source as its child")) {
        return false;
    }
    server.pop_event(second_owner);
    if (!expect(server.inject_input(3, 41, 90, 70) ==
                    xmin::server::EventDelivery::delivered,
                "focused key release outside the focus subtree was lost")) {
        return false;
    }
    server.pop_event(second_owner);
    stored_parent->event_masks.erase(second_owner);
    server.window(xmin::server::root_window_id)->event_masks.emplace(
        first_owner, key_press_mask | key_release_mask);
    if (!expect(server.inject_input(2, 42, 90, 70) ==
                    xmin::server::EventDelivery::no_recipient &&
                    server.inject_input(3, 42, 90, 70) ==
                    xmin::server::EventDelivery::no_recipient &&
                    !server.has_pending_event(first_owner),
                "keyboard propagation escaped above the focus window")) {
        return false;
    }
    server.window(xmin::server::root_window_id)->event_masks.erase(first_owner);
    stored_parent->event_masks.emplace(
        second_owner, key_press_mask | key_release_mask);
    if (!expect(server.inject_input(6, 0, 18, 17) ==
                    xmin::server::EventDelivery::no_recipient,
                "pointer restoration unexpectedly delivered an event")) {
        return false;
    }

    server.input().keyboard_grab = xmin::server::ActiveGrab{
        first_owner, xmin::server::root_window_id, 0, server.current_time(),
        key_press_mask, 1, 1, false};
    if (!expect(server.inject_input(2, 39, 18, 17) ==
                    xmin::server::EventDelivery::delivered,
                "active keyboard grab did not receive a key press")) {
        return false;
    }
    queued = server.next_event(first_owner);
    key = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CoreInputEvent>(queued);
    if (!expect(key != nullptr && key->detail == 39 &&
                    key->event == xmin::server::root_window_id &&
                    key->child == parent_id && key->sequence == 31 &&
                    !server.has_pending_event(second_owner),
                "active keyboard grab did not override normal selection")) {
        return false;
    }
    server.pop_event(first_owner);
    if (!expect(server.inject_input(3, 39, 18, 17) ==
                    xmin::server::EventDelivery::no_recipient,
                "unselected grabbed key release was delivered")) {
        return false;
    }
    server.input().keyboard_grab.reset();

    xmin::server::PassiveGrab passive;
    passive.kind = xmin::server::PassiveGrabKind::key;
    passive.details = xmin::server::passive_grab_details(passive.kind, 40);
    passive.modifiers = xmin::server::passive_grab_modifiers(0);
    passive.owner = first_owner;
    passive.window = parent_id;
    passive.event_mask = key_press_mask | key_release_mask;
    if (!expect(server.add_passive_grab(std::move(passive)) ==
                    xmin::server::PassiveGrabUpdate::updated,
                "passive input-routing grab insertion failed") ||
        !expect(server.inject_input(2, 40, 18, 17) ==
                    xmin::server::EventDelivery::delivered &&
                    server.input().keyboard_grab &&
                    server.input().keyboard_grab->passive &&
                    server.input().keyboard_grab->passive_detail == 40,
                "passive key grab did not activate")) {
        return false;
    }
    queued = server.next_event(first_owner);
    key = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CoreInputEvent>(queued);
    if (!expect(key != nullptr && key->detail == 40 &&
                    key->event == parent_id && key->child == child_id,
                "passive grab press used the wrong event path")) {
        return false;
    }
    server.pop_event(first_owner);
    if (!expect(server.inject_input(3, 40, 18, 17) ==
                    xmin::server::EventDelivery::delivered &&
                    !server.input().keyboard_grab,
                "passive key grab did not release")) {
        return false;
    }
    queued = server.next_event(first_owner);
    key = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CoreInputEvent>(queued);
    if (!expect(key != nullptr && key->type == 3 && key->detail == 40,
                "passive grab release event was not delivered")) {
        return false;
    }
    server.pop_event(first_owner);

    stored_child->event_masks.emplace(
        second_owner, button_press_mask | button_release_mask);
    xmin::server::PassiveGrab pointer;
    pointer.kind = xmin::server::PassiveGrabKind::button;
    pointer.details = xmin::server::passive_grab_details(pointer.kind, 3);
    pointer.modifiers = xmin::server::passive_grab_modifiers(0);
    pointer.owner = first_owner;
    pointer.window = child_id;
    pointer.event_mask = button_press_mask;
    pointer.pointer_mode = 0;
    pointer.owner_events = true;
    if (!expect(server.add_passive_grab(std::move(pointer)) ==
                    xmin::server::PassiveGrabUpdate::updated &&
                    server.inject_input(4, 3, 18, 17) ==
                    xmin::server::EventDelivery::delivered &&
                    server.input().pointer_grab &&
                    server.input().pointer_grab->passive &&
                    server.input().frozen_pointer_event.has_value(),
                "synchronous passive pointer grab did not freeze")) {
        return false;
    }
    queued = server.next_event(first_owner);
    const auto *button = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CoreInputEvent>(queued);
    if (!expect(button != nullptr && button->type == 4 &&
                    button->detail == 3 && button->event == child_id &&
                    !server.has_pending_event(second_owner),
                "synchronous passive grab press used the wrong route")) {
        return false;
    }
    server.pop_event(first_owner);
    if (!expect(server.inject_input(5, 3, 18, 17) ==
                    xmin::server::EventDelivery::no_recipient &&
                    server.input().pointer_grab &&
                    server.input().frozen_pointer_event &&
                    server.input().frozen_pointer_event->pending.size() == 1 &&
                    server.input().pressed_buttons.test(3),
                "synchronous pointer grab did not queue the release")) {
        return false;
    }
    if (!expect(server.allow_events(first_owner, 2, 0) ==
                    xmin::server::EventDelivery::delivered &&
                    !server.input().pointer_grab &&
                    !server.input().frozen_pointer_event &&
                    server.input().pressed_buttons.none(),
                "ReplayPointer did not release the passive grab")) {
        return false;
    }
    queued = server.next_event(second_owner);
    button = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CoreInputEvent>(queued);
    if (!expect(button != nullptr && button->type == 4 &&
                    button->detail == 3 && button->event == child_id &&
                    button->state == 0,
                "ReplayPointer did not deliver the activating event")) {
        return false;
    }
    server.pop_event(second_owner);
    queued = server.next_event(second_owner);
    button = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CoreInputEvent>(queued);
    return expect(button != nullptr && button->type == 5 &&
                      button->detail == 3 &&
                      button->state == (1U << 10),
                  "replayed pointer release metadata is wrong");
}

bool
test_key_repeat_timers()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t key_mask = (1U << 0) | (1U << 1);
    constexpr std::uint8_t keycode = 96;
    ManualClock clock;
    xmin::server::ServerState server(100, 80, clock);
    if (!expect(server.register_client(owner),
                "repeat-timer client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 59);
    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window,
                    xmin::server::root_window_id, 0, 0) ==
                    xmin::server::FocusUpdate::updated,
                "repeat-timer focus setup failed")) {
        return false;
    }
    server.window(xmin::server::root_window_id)->event_masks.emplace(
        owner, key_mask);

    const auto key = [&](std::uint8_t type,
                         std::optional<std::uint32_t> time = std::nullopt) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::CoreInputEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == keycode &&
            value->event == xmin::server::root_window_id &&
            value->state == 0 && value->sequence == 59 &&
            (!time || value->time == *time);
        const std::uint32_t event_time = value == nullptr ? 0 : value->time;
        server.pop_event(owner);
        return std::pair<bool, std::uint32_t>{matches, event_time};
    };

    if (!expect(server.inject_input(2, keycode, 50, 40) ==
                    xmin::server::EventDelivery::delivered,
                "repeat-timer key press failed")) {
        return false;
    }
    const auto initial_press = key(2);
    if (!expect(initial_press.first &&
                    server.timer_timeout_milliseconds() == 660,
                "repeat delay was not armed")) {
        return false;
    }
    clock.advance(std::chrono::milliseconds{659});
    if (!expect(server.process_timers() ==
                    xmin::server::EventDelivery::no_recipient &&
                    !server.has_pending_event(owner) &&
                    server.timer_timeout_milliseconds() == 1,
                "repeat fired before its initial deadline")) {
        return false;
    }
    clock.advance(std::chrono::milliseconds{1});
    if (!expect(server.process_timers() ==
                    xmin::server::EventDelivery::delivered,
                "repeat deadline did not fire")) {
        return false;
    }
    const auto first_release = key(3);
    const auto first_repeat = key(2, first_release.second);
    if (!expect(first_release.first && first_repeat.first &&
                    first_release.second > initial_press.second &&
                    (server.input().pressed_keys[keycode >> 3] &
                     (1U << (keycode & 7U))) != 0 &&
                    server.timer_timeout_milliseconds() == 40,
                "repeat pair changed persistent key state or timestamps")) {
        return false;
    }

    clock.advance(std::chrono::milliseconds{80});
    if (!expect(server.process_timers() ==
                    xmin::server::EventDelivery::delivered,
                "repeat interval catch-up failed")) {
        return false;
    }
    for (std::size_t pair = 0; pair < 2; ++pair) {
        const auto release = key(3);
        const auto press = key(2, release.second);
        if (!expect(release.first && press.first,
                    "repeat catch-up pair is malformed")) {
            return false;
        }
    }
    if (!expect(!server.has_pending_event(owner) &&
                    server.inject_input(3, keycode, 50, 40) ==
                        xmin::server::EventDelivery::delivered &&
                    key(3).first &&
                    server.timer_timeout_milliseconds() == -1,
                "physical release did not cancel repeat")) {
        return false;
    }

    server.input().global_auto_repeat = false;
    server.update_repeat_controls();
    if (!expect(server.inject_input(2, keycode, 50, 40) ==
                    xmin::server::EventDelivery::delivered &&
                    key(2).first &&
                    server.timer_timeout_milliseconds() == -1 &&
                    server.inject_input(3, keycode, 50, 40) ==
                        xmin::server::EventDelivery::delivered &&
                    key(3).first,
                "global repeat disable was ignored")) {
        return false;
    }
    server.input().global_auto_repeat = true;
    server.input().auto_repeats[keycode >> 3] &=
        static_cast<std::uint8_t>(~(1U << (keycode & 7U)));
    server.update_repeat_controls();
    if (!expect(server.inject_input(2, keycode, 50, 40) ==
                    xmin::server::EventDelivery::delivered &&
                    key(2).first &&
                    server.timer_timeout_milliseconds() == -1 &&
                    server.inject_input(3, keycode, 50, 40) ==
                        xmin::server::EventDelivery::delivered &&
                    key(3).first,
                "per-key repeat disable was ignored")) {
        return false;
    }
    server.input().auto_repeats[keycode >> 3] |=
        static_cast<std::uint8_t>(1U << (keycode & 7U));

    if (!expect(server.inject_input(2, keycode, 50, 40) ==
                    xmin::server::EventDelivery::delivered &&
                    key(2).first,
                "repeat queue-pressure key press failed")) {
        return false;
    }
    xmin::server::ClientMessageEvent message;
    message.window = xmin::server::root_window_id;
    for (std::size_t count = 0;
         count + 1 < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(
                        xmin::server::root_window_id, key_mask, false,
                        message) ==
                        xmin::server::EventDelivery::delivered,
                    "repeat queue-pressure setup failed")) {
            return false;
        }
    }
    clock.advance(xmin::server::default_repeat_delay);
    if (!expect(server.process_timers() ==
                    xmin::server::EventDelivery::queue_full &&
                    server.timer_timeout_milliseconds() == 40 &&
                    (server.input().pressed_keys[keycode >> 3] &
                     (1U << (keycode & 7U))) != 0,
                "repeat queue failure changed timer or key state")) {
        return false;
    }
    std::size_t queued_count = 0;
    while (server.has_pending_event(owner)) {
        const auto *queued = server.next_event(owner);
        if (!expect(queued != nullptr &&
                        std::holds_alternative<
                            xmin::server::ClientMessageEvent>(*queued),
                    "repeat queue failure left a partial event pair")) {
            return false;
        }
        server.pop_event(owner);
        ++queued_count;
    }
    return expect(
        queued_count + 1 == xmin::server::maximum_pending_events_per_client &&
            server.inject_input(3, keycode, 50, 40) ==
                xmin::server::EventDelivery::delivered &&
            key(3).first && server.timer_timeout_milliseconds() == -1,
        "repeat queue failure changed the existing event count");
}

bool
test_xkb_state()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint8_t lock_mask = 1U << 1;
    ManualClock clock;
    xmin::server::ServerState server(100, 80, clock);
    if (!expect(server.register_client(owner),
                "XKB client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 73);
    xmin::server::XkbEventSelection selection;
    selection.owner = owner;
    selection.events = (1U << 2) | (1U << 3);
    selection.state = 0x3fff;
    selection.controls = 0xffffffffU;
    if (!expect(server.select_xkb_events(selection) ==
                    xmin::server::XkbUpdate::updated,
                "XKB event selection failed") ||
        !expect(server.latch_lock_xkb(
                    lock_mask, lock_mask, false, 0, 0, false, 0,
                    140, 5) == xmin::server::XkbUpdate::updated,
                "XKB lock update failed")) {
        return false;
    }
    const auto *queued = server.next_event(owner);
    const auto *state = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::XkbStateNotifyEvent>(queued);
    if (!expect(state != nullptr && state->locked_mods == lock_mask &&
                    state->mods == lock_mask &&
                    (state->changed & (1U << 3)) != 0 &&
                    state->request_major == 140 &&
                    state->request_minor == 5 && state->sequence == 73 &&
                    server.xkb_indicator_state() == 1,
                "XKB StateNotify or indicator state is malformed")) {
        return false;
    }
    server.pop_event(owner);

    auto controls = server.input().xkb.controls;
    controls.repeat_delay = 500;
    controls.repeat_interval = 30;
    if (!expect(server.set_xkb_controls(
                    controls, 1U << 30, 0, 140, 7) ==
                    xmin::server::XkbUpdate::updated,
                "XKB controls update failed")) {
        return false;
    }
    queued = server.next_event(owner);
    const auto *control_event = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::XkbControlsNotifyEvent>(queued);
    if (!expect(control_event != nullptr &&
                    control_event->changed == (1U << 30) &&
                    control_event->enabled == controls.enabled &&
                    control_event->request_major == 140 &&
                    control_event->request_minor == 7 &&
                    server.input().xkb.controls.repeat_delay == 500,
                "XKB ControlsNotify is malformed")) {
        return false;
    }
    server.pop_event(owner);

    server.window(xmin::server::root_window_id)->event_masks.emplace(owner, 1);
    xmin::server::ClientMessageEvent message;
    message.window = xmin::server::root_window_id;
    for (std::size_t count = 0;
         count < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(
                        xmin::server::root_window_id, 1, false, message) ==
                        xmin::server::EventDelivery::delivered,
                    "XKB queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.latch_lock_xkb(
                    lock_mask, 0, false, 0, 0, false, 0, 140, 5) ==
                    xmin::server::XkbUpdate::queue_full &&
                    server.xkb_state().locked_mods == lock_mask,
                "failed XKB event delivery committed state")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);
    server.disconnect_client(owner);
    return expect(server.xkb_selection(owner) == nullptr,
                  "disconnect retained an XKB event selection");
}

bool
test_xkb_detectable_repeat()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint8_t keycode = 96;
    ManualClock clock;
    xmin::server::ServerState server(100, 80, clock);
    if (!expect(server.register_client(owner) &&
                    server.set_input_focus(
                        xmin::server::FocusKind::window,
                        xmin::server::root_window_id, 0, 0) ==
                        xmin::server::FocusUpdate::updated,
                "detectable-repeat setup failed")) {
        return false;
    }
    server.window(xmin::server::root_window_id)->event_masks.emplace(
        owner, (1U << 0) | (1U << 1));
    if (!expect(server.set_xkb_client_flags(owner, 1) ==
                    xmin::server::XkbUpdate::updated &&
                    server.xkb_client_flags(owner) == 1,
                "detectable repeat flag was not retained") ||
        !expect(server.inject_input(2, keycode, 50, 40) ==
                    xmin::server::EventDelivery::delivered,
                "detectable-repeat key press failed")) {
        return false;
    }
    server.pop_event(owner);
    clock.advance(xmin::server::default_repeat_delay);
    if (!expect(server.process_timers() ==
                    xmin::server::EventDelivery::delivered,
                "detectable repeat timer did not fire")) {
        return false;
    }
    const auto *queued = server.next_event(owner);
    const auto *press = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CoreInputEvent>(queued);
    if (!expect(press != nullptr && press->type == 2 &&
                    press->detail == keycode,
                "detectable repeat did not suppress its release")) {
        return false;
    }
    server.pop_event(owner);
    return expect(!server.has_pending_event(owner) &&
                      server.inject_input(3, keycode, 50, 40) ==
                          xmin::server::EventDelivery::delivered,
                  "detectable repeat left a partial pair");
}

bool
test_xi2_state()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t property = 69;
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(owner),
                "XI2 client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 91);
    xmin::server::Xi2EventSelection selection;
    selection.owner = owner;
    selection.window = xmin::server::root_window_id;
    selection.masks.push_back({
        xmin::server::xi2_all_master_devices,
        {(1U << 2) | (1U << 6) | (1U << 9) | (1U << 10) | (1U << 12) |
         (1U << 13) | (1U << 17)}});
    if (!expect(server.select_xi2_events(std::move(selection)) ==
                    xmin::server::Xi2Update::updated,
                "XI2 event selection failed") ||
        !expect(server.inject_input(6, 0, 37, 29) ==
                    xmin::server::EventDelivery::delivered,
                "XI2 motion did not share core pointer state")) {
        return false;
    }
    const auto *queued = server.next_event(owner);
    const auto *raw = queued == nullptr
        ? nullptr : std::get_if<xmin::server::Xi2RawEvent>(queued);
    if (!expect(raw != nullptr && raw->event_type == 17 &&
                    raw->device == xmin::server::xi2_pointer_device_id &&
                    raw->source == xmin::server::xi2_pointer_device_id &&
                    raw->root_x == 37 && raw->root_y == 29 &&
                    raw->sequence == 91,
                "XI2 RawMotion metadata is malformed")) {
        return false;
    }
    server.pop_event(owner);
    queued = server.next_event(owner);
    const auto *motion = queued == nullptr
        ? nullptr : std::get_if<xmin::server::Xi2DeviceEvent>(queued);
    if (!expect(motion != nullptr && motion->event_type == 6 &&
                    motion->device == xmin::server::xi2_pointer_device_id &&
                    motion->source == xmin::server::xi2_pointer_device_id &&
                    motion->root == xmin::server::root_window_id &&
                    motion->event == xmin::server::root_window_id &&
                    motion->root_x == 37 && motion->root_y == 29 &&
                    server.input().pointer_x == 37 &&
                    server.input().pointer_y == 29,
                "XI2 Motion is not a view of core pointer state")) {
        return false;
    }
    server.pop_event(owner);

    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window,
                    xmin::server::root_window_id, 0, 0) ==
                    xmin::server::FocusUpdate::updated,
                "XI2 keyboard focus setup failed")) {
        return false;
    }
    bool saw_focus_out = false;
    bool saw_focus_in = false;
    while (server.has_pending_event(owner)) {
        const auto *focus = std::get_if<xmin::server::Xi2CrossingEvent>(
            server.next_event(owner));
        if (!expect(focus != nullptr &&
                        focus->device ==
                            xmin::server::xi2_keyboard_device_id &&
                        focus->event == xmin::server::root_window_id,
                    "XI2 focus metadata is malformed")) {
            return false;
        }
        saw_focus_out = saw_focus_out || focus->event_type == 10;
        saw_focus_in = saw_focus_in || focus->event_type == 9;
        server.pop_event(owner);
    }
    if (!expect(saw_focus_out && saw_focus_in,
                "XI2 focus transition omitted an endpoint")) {
        return false;
    }
    if (!expect(server.inject_input(2, 38, 37, 29) ==
                    xmin::server::EventDelivery::delivered,
                "XI2 key press was not delivered")) {
        return false;
    }
    raw = std::get_if<xmin::server::Xi2RawEvent>(server.next_event(owner));
    if (!expect(raw != nullptr && raw->event_type == 13 &&
                    raw->device == xmin::server::xi2_keyboard_device_id,
                "XI2 RawKeyPress used the wrong device")) {
        return false;
    }
    server.pop_event(owner);
    const auto *key = std::get_if<xmin::server::Xi2DeviceEvent>(
        server.next_event(owner));
    if (!expect(key != nullptr && key->event_type == 2 &&
                    key->detail == 38 &&
                    key->device == xmin::server::xi2_keyboard_device_id,
                "XI2 KeyPress did not share keyboard routing")) {
        return false;
    }
    server.pop_event(owner);
    static_cast<void>(server.inject_input(3, 38, 37, 29));
    while (server.has_pending_event(owner))
        server.pop_event(owner);

    xmin::server::PropertyValue value;
    value.type = 6;
    value.format = 32;
    value.data = {1, 0, 0, 0};
    if (!expect(server.set_xi2_property(
                    xmin::server::xi2_keyboard_device_id, property, value) ==
                    xmin::server::Xi2Update::updated,
                "XI2 property creation failed")) {
        return false;
    }
    const auto *property_event =
        std::get_if<xmin::server::Xi2PropertyEvent>(server.next_event(owner));
    if (!expect(property_event != nullptr &&
                    property_event->device ==
                        xmin::server::xi2_keyboard_device_id &&
                    property_event->property == property &&
                    property_event->what == 1,
                "XI2 PropertyEvent is malformed")) {
        return false;
    }
    server.pop_event(owner);

    server.window(xmin::server::root_window_id)->event_masks.emplace(owner, 1);
    xmin::server::ClientMessageEvent message;
    message.window = xmin::server::root_window_id;
    for (std::size_t count = 0;
         count < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(
                        xmin::server::root_window_id, 1, false, message) ==
                        xmin::server::EventDelivery::delivered,
                    "XI2 queue-pressure setup failed")) {
            return false;
        }
    }
    value.data = {2, 0, 0, 0};
    if (!expect(server.set_xi2_property(
                    xmin::server::xi2_keyboard_device_id, property, value) ==
                    xmin::server::Xi2Update::queue_full &&
                    server.xi2_properties(
                        xmin::server::xi2_keyboard_device_id).at(property)
                        .data.front() == 1,
                "failed XI2 event delivery committed property state")) {
        return false;
    }
    if (!expect(server.delete_xi2_property(
                    xmin::server::xi2_keyboard_device_id, property) ==
                    xmin::server::Xi2Update::queue_full &&
                    server.xi2_properties(
                        xmin::server::xi2_keyboard_device_id).count(property) == 1,
                "failed XI2 delete event removed property state")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);
    if (!expect(server.delete_xi2_property(
                    xmin::server::xi2_keyboard_device_id, property) ==
                    xmin::server::Xi2Update::updated &&
                    server.xi2_properties(
                        xmin::server::xi2_keyboard_device_id).count(property) == 0,
                "XI2 property deletion failed")) {
        return false;
    }
    property_event = std::get_if<xmin::server::Xi2PropertyEvent>(
        server.next_event(owner));
    if (!expect(property_event != nullptr && property_event->what == 0,
                "XI2 property deletion event is malformed")) {
        return false;
    }
    server.pop_event(owner);
    server.disconnect_client(owner);
    return expect(server.xi2_selection(
                      owner, xmin::server::root_window_id) == nullptr,
                  "disconnect retained an XI2 event selection");
}

bool
test_crossing_events()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t left = owner | 1U;
    constexpr std::uint32_t left_child = owner | 2U;
    constexpr std::uint32_t right = owner | 3U;
    constexpr std::uint32_t right_child = owner | 4U;
    constexpr std::uint32_t crossing_mask = (1U << 4) | (1U << 5);
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(owner),
                "crossing client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 71);

    const auto add_window = [&](std::uint32_t id, std::uint32_t parent,
                                std::int16_t x, std::int16_t y,
                                std::uint16_t width,
                                std::uint16_t height) {
        xmin::server::WindowRecord window;
        window.id = id;
        window.parent = parent;
        window.x = x;
        window.y = y;
        window.width = width;
        window.height = height;
        if (!server.add_window(std::move(window), owner))
            return false;
        auto *stored = server.window(id);
        static_cast<void>(server.set_window_mapped(*stored, true));
        stored->event_masks.emplace(owner, crossing_mask);
        return true;
    };
    server.window(xmin::server::root_window_id)->event_masks.emplace(
        owner, crossing_mask);
    if (!expect(add_window(left, xmin::server::root_window_id,
                           0, 0, 40, 40) &&
                    add_window(left_child, left, 5, 5, 10, 10) &&
                    add_window(right, xmin::server::root_window_id,
                               50, 0, 40, 40) &&
                    add_window(right_child, right, 5, 5, 10, 10),
                "crossing window insertion failed")) {
        return false;
    }

    const auto crossing = [&](std::uint8_t type, std::uint8_t detail,
                              std::uint32_t event, std::uint32_t child,
                              std::int16_t event_x,
                              std::int16_t event_y) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::CrossingEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->root ==
                xmin::server::root_window_id &&
            value->event == event && value->child == child &&
            value->event_x == event_x && value->event_y == event_y &&
            value->mode == 0 && value->same_screen && value->focus &&
            value->sequence == 71;
        server.pop_event(owner);
        return matches;
    };

    if (!expect(server.inject_input(6, 0, 7, 7) ==
                    xmin::server::EventDelivery::delivered,
                "descendant crossing was not delivered") ||
        !expect(crossing(8, 2, xmin::server::root_window_id, 0, 7, 7) &&
                    crossing(7, 1, left, left_child, 7, 7) &&
                    crossing(7, 0, left_child, 0, 2, 2) &&
                    !server.has_pending_event(owner),
                "descendant crossing path is wrong")) {
        return false;
    }

    if (!expect(server.inject_input(6, 0, 57, 7) ==
                    xmin::server::EventDelivery::delivered,
                "nonlinear crossing was not delivered") ||
        !expect(crossing(8, 3, left_child, 0, 52, 2) &&
                    crossing(8, 4, left, left_child, 57, 7) &&
                    crossing(7, 4, right, right_child, 7, 7) &&
                    crossing(7, 3, right_child, 0, 2, 2) &&
                    !server.has_pending_event(owner),
                "nonlinear crossing path is wrong")) {
        return false;
    }

    if (!expect(server.inject_input(6, 0, 95, 70) ==
                    xmin::server::EventDelivery::delivered,
                "ancestor crossing was not delivered") ||
        !expect(crossing(8, 0, right_child, 0, 40, 65) &&
                    crossing(8, 1, right, right_child, 45, 70) &&
                    crossing(7, 2, xmin::server::root_window_id, 0, 95, 70) &&
                    !server.has_pending_event(owner),
                "ancestor crossing path is wrong")) {
        return false;
    }

    xmin::server::ClientMessageEvent message;
    message.window = left;
    for (std::size_t count = 0;
         count + 1 < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(left, 0, false, message) ==
                        xmin::server::EventDelivery::delivered,
                    "crossing queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.inject_input(6, 0, 7, 7) ==
                    xmin::server::EventDelivery::queue_full &&
                    server.input().pointer_x == 95 &&
                    server.input().pointer_y == 70,
                "partial crossing escaped an atomic queue failure")) {
        return false;
    }
    std::size_t queued = 0;
    while (server.has_pending_event(owner)) {
        const auto *event = server.next_event(owner);
        if (!expect(event != nullptr &&
                        std::holds_alternative<xmin::server::ClientMessageEvent>(
                            *event),
                    "queue failure left a partial crossing event")) {
            return false;
        }
        server.pop_event(owner);
        ++queued;
    }
    return expect(
        queued + 1 == xmin::server::maximum_pending_events_per_client,
        "queue failure changed the preexisting event count");
}

bool
test_automatic_pointer_grab()
{
    constexpr std::uint32_t first_owner = 0x00200000;
    constexpr std::uint32_t second_owner = 0x00400000;
    constexpr std::uint32_t parent = first_owner | 1U;
    constexpr std::uint32_t child = first_owner | 2U;
    constexpr std::uint32_t button_masks = (1U << 2) | (1U << 3);
    constexpr std::uint32_t crossing_masks = (1U << 4) | (1U << 5);
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(first_owner) &&
                    server.register_client(second_owner),
                "automatic-grab client registration failed")) {
        return false;
    }

    xmin::server::WindowRecord parent_window;
    parent_window.id = parent;
    parent_window.parent = xmin::server::root_window_id;
    parent_window.x = 10;
    parent_window.y = 10;
    parent_window.width = 40;
    parent_window.height = 40;
    xmin::server::WindowRecord child_window;
    child_window.id = child;
    child_window.parent = parent;
    child_window.x = 5;
    child_window.y = 5;
    child_window.width = 20;
    child_window.height = 20;
    if (!expect(server.add_window(std::move(parent_window), first_owner) &&
                    server.add_window(std::move(child_window), first_owner),
                "automatic-grab window insertion failed")) {
        return false;
    }
    auto *stored_parent = server.window(parent);
    auto *stored_child = server.window(child);
    static_cast<void>(server.set_window_mapped(*stored_parent, true));
    static_cast<void>(server.set_window_mapped(*stored_child, true));
    if (!expect(server.inject_input(6, 0, 18, 18) ==
                    xmin::server::EventDelivery::no_recipient,
                "automatic-grab pointer setup delivered an event")) {
        return false;
    }
    stored_parent->event_masks.emplace(
        first_owner, button_masks | crossing_masks);
    stored_child->event_masks.emplace(second_owner, crossing_masks);
    if (!expect(server.inject_input(4, 1, 18, 18) ==
                    xmin::server::EventDelivery::delivered &&
                    server.input().pointer_grab &&
                    server.input().pointer_grab->automatic &&
                    server.input().pointer_grab->owner == first_owner &&
                    server.input().pointer_grab->window == parent,
                "normal button press did not activate an automatic grab")) {
        return false;
    }
    const auto *queued = server.next_event(first_owner);
    const auto *button = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CoreInputEvent>(queued);
    if (!expect(button != nullptr && button->type == 4 &&
                    button->detail == 1 && button->event == parent &&
                    button->child == child && button->state == 0,
                "automatic-grab button press metadata is wrong")) {
        return false;
    }
    server.pop_event(first_owner);
    queued = server.next_event(first_owner);
    const auto *crossing = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CrossingEvent>(queued);
    if (!expect(crossing != nullptr && crossing->type == 7 &&
                    crossing->detail == 2 && crossing->event == parent &&
                    crossing->mode == 1,
                "automatic-grab enter transition is wrong")) {
        return false;
    }
    server.pop_event(first_owner);
    queued = server.next_event(second_owner);
    crossing = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CrossingEvent>(queued);
    if (!expect(crossing != nullptr && crossing->type == 8 &&
                    crossing->detail == 0 && crossing->event == child &&
                    crossing->mode == 1,
                "automatic-grab leave transition is wrong")) {
        return false;
    }
    server.pop_event(second_owner);

    if (!expect(server.inject_input(4, 2, 18, 18) ==
                    xmin::server::EventDelivery::delivered &&
                    server.inject_input(5, 1, 18, 18) ==
                    xmin::server::EventDelivery::delivered &&
                    server.input().pointer_grab &&
                    server.input().pointer_grab->automatic,
                "automatic grab ended before the final button release")) {
        return false;
    }
    server.pop_event(first_owner);
    server.pop_event(first_owner);
    if (!expect(server.inject_input(5, 2, 18, 18) ==
                    xmin::server::EventDelivery::delivered &&
                    !server.input().pointer_grab &&
                    server.input().pressed_buttons.none(),
                "automatic grab survived the final button release")) {
        return false;
    }
    queued = server.next_event(first_owner);
    button = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CoreInputEvent>(queued);
    if (!expect(button != nullptr && button->type == 5 &&
                    button->detail == 2 && button->state == (1U << 9),
                "final grabbed button release has the wrong state")) {
        return false;
    }
    server.pop_event(first_owner);
    queued = server.next_event(first_owner);
    crossing = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CrossingEvent>(queued);
    if (!expect(crossing != nullptr && crossing->type == 8 &&
                    crossing->detail == 2 && crossing->event == parent &&
                    crossing->mode == 2,
                "automatic-ungrab leave transition is wrong")) {
        return false;
    }
    server.pop_event(first_owner);
    queued = server.next_event(second_owner);
    crossing = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CrossingEvent>(queued);
    return expect(crossing != nullptr && crossing->type == 7 &&
                      crossing->detail == 0 && crossing->event == child &&
                      crossing->mode == 2,
                  "automatic-ungrab enter transition is wrong");
}

bool
test_focus_events()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t parent = owner | 1U;
    constexpr std::uint32_t child = owner | 2U;
    constexpr std::uint32_t sibling = owner | 3U;
    constexpr std::uint32_t focus_mask = 1U << 21;
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(owner),
                "focus-event client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 83);
    const auto add_window = [&](std::uint32_t id, std::uint32_t parent_id,
                                std::int16_t x, std::int16_t y,
                                std::uint16_t width,
                                std::uint16_t height) {
        xmin::server::WindowRecord window;
        window.id = id;
        window.parent = parent_id;
        window.x = x;
        window.y = y;
        window.width = width;
        window.height = height;
        if (!server.add_window(std::move(window), owner))
            return false;
        static_cast<void>(server.set_window_mapped(*server.window(id), true));
        return true;
    };
    if (!expect(add_window(parent, xmin::server::root_window_id,
                           5, 5, 40, 40) &&
                    add_window(child, parent, 5, 5, 20, 20) &&
                    add_window(sibling, xmin::server::root_window_id,
                               60, 5, 30, 30) &&
                    server.inject_input(6, 0, 15, 15) ==
                        xmin::server::EventDelivery::no_recipient,
                "focus-event window setup failed")) {
        return false;
    }
    server.window(xmin::server::root_window_id)->event_masks.emplace(
        owner, focus_mask);
    server.window(parent)->event_masks.emplace(owner, focus_mask);
    server.window(child)->event_masks.emplace(owner, focus_mask);
    server.window(sibling)->event_masks.emplace(owner, focus_mask);

    const auto focus = [&](std::uint8_t type, std::uint8_t detail,
                           std::uint32_t event) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::FocusEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == event &&
            value->mode == 0 && value->sequence == 83;
        server.pop_event(owner);
        return matches;
    };
    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window, child, 0, 0) ==
                    xmin::server::FocusUpdate::updated &&
                    focus(10, 5, child) && focus(10, 5, parent) &&
                    focus(10, 5, xmin::server::root_window_id) &&
                    focus(10, 6, xmin::server::root_window_id) &&
                    focus(9, 4, xmin::server::root_window_id) &&
                    focus(9, 4, parent) && focus(9, 3, child),
                "PointerRoot-to-window focus path is wrong")) {
        return false;
    }
    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window, sibling, 0, 0) ==
                    xmin::server::FocusUpdate::updated &&
                    focus(10, 3, child) && focus(10, 4, parent) &&
                    focus(9, 3, sibling),
                "nonlinear focus path is wrong")) {
        return false;
    }
    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window, child, 0, 0) ==
                    xmin::server::FocusUpdate::updated &&
                    focus(10, 3, sibling) && focus(9, 4, parent) &&
                    focus(9, 3, child),
                "reverse nonlinear focus path is wrong")) {
        return false;
    }
    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window,
                    xmin::server::root_window_id, 0, 0) ==
                    xmin::server::FocusUpdate::updated &&
                    focus(10, 0, child) && focus(10, 1, parent) &&
                    focus(9, 2, xmin::server::root_window_id),
                "ancestor focus path is wrong")) {
        return false;
    }
    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::none, 0, 0, 0) ==
                    xmin::server::FocusUpdate::updated &&
                    focus(10, 5, child) && focus(10, 5, parent) &&
                    focus(10, 3, xmin::server::root_window_id) &&
                    focus(9, 7, xmin::server::root_window_id) &&
                    server.set_input_focus(
                        xmin::server::FocusKind::pointer_root, 0, 0, 0) ==
                        xmin::server::FocusUpdate::updated &&
                    focus(10, 7, xmin::server::root_window_id) &&
                    focus(9, 6, xmin::server::root_window_id) &&
                    focus(9, 5, xmin::server::root_window_id) &&
                    focus(9, 5, parent) && focus(9, 5, child) &&
                    !server.has_pending_event(owner),
                "None/PointerRoot focus path is wrong")) {
        return false;
    }
    xmin::server::ClientMessageEvent message;
    message.window = sibling;
    for (std::size_t count = 0;
         count + 1 < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(
                        sibling, 0, false, message) ==
                        xmin::server::EventDelivery::delivered,
                    "focus queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window, child, 0, 0) ==
                    xmin::server::FocusUpdate::queue_full &&
                    server.input().focus.kind ==
                        xmin::server::FocusKind::pointer_root,
                "partial focus path escaped an atomic queue failure")) {
        return false;
    }
    std::size_t queued_count = 0;
    while (server.has_pending_event(owner)) {
        const auto *event = server.next_event(owner);
        if (!expect(event != nullptr &&
                        std::holds_alternative<xmin::server::ClientMessageEvent>(
                            *event),
                    "queue failure left a partial focus event")) {
            return false;
        }
        server.pop_event(owner);
        ++queued_count;
    }
    return expect(
        queued_count + 1 == xmin::server::maximum_pending_events_per_client,
        "focus queue failure changed the preexisting event count");
}

bool
test_structure_mapping_notifications()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t observer = 0x00400000;
    constexpr std::uint32_t child = owner | 1U;
    constexpr std::uint32_t exposure_mask = 1U << 15;
    constexpr std::uint32_t visibility_change_mask = 1U << 16;
    constexpr std::uint32_t structure_notify_mask = 1U << 17;
    constexpr std::uint32_t substructure_notify_mask = 1U << 19;
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(owner) &&
                    server.register_client(observer),
                "mapping-notify client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 107);
    server.note_client_sequence(observer, 109);
    server.window(xmin::server::root_window_id)->event_masks.emplace(
        observer, substructure_notify_mask);

    xmin::server::WindowRecord window;
    window.id = child;
    window.parent = xmin::server::root_window_id;
    window.x = 5;
    window.y = 7;
    window.width = 20;
    window.height = 15;
    if (!expect(server.add_window(std::move(window), owner),
                "mapping-notify window insertion failed")) {
        return false;
    }
    {
        const auto *queued = server.next_event(observer);
        const auto *event = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::CreateNotifyEvent>(queued);
        if (!expect(event != nullptr &&
                        event->parent == xmin::server::root_window_id &&
                        event->window == child && event->x == 5 &&
                        event->y == 7 && event->width == 20 &&
                        event->height == 15 && event->border_width == 0 &&
                        !event->override_redirect && event->sequence == 109,
                    "CreateNotify routing or payload is wrong")) {
            return false;
        }
        server.pop_event(observer);
    }
    server.window(child)->event_masks.emplace(
        owner,
        exposure_mask | structure_notify_mask | visibility_change_mask);

    const auto structure_map = [&]() {
        const auto *queued = server.next_event(owner);
        const auto *event = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::MapNotifyEvent>(queued);
        const bool matches = event != nullptr && event->event == child &&
            event->window == child && !event->override_redirect &&
            event->sequence == 107;
        server.pop_event(owner);
        return matches;
    };
    const auto substructure_map = [&]() {
        const auto *queued = server.next_event(observer);
        const auto *event = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::MapNotifyEvent>(queued);
        const bool matches = event != nullptr &&
            event->event == xmin::server::root_window_id &&
            event->window == child && !event->override_redirect &&
            event->sequence == 109;
        server.pop_event(observer);
        return matches;
    };
    const auto visibility_map = [&]() {
        const auto *queued = server.next_event(owner);
        const auto *event = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::VisibilityNotifyEvent>(queued);
        const bool matches = event != nullptr && event->window == child &&
            event->state == 0 && event->sequence == 107;
        server.pop_event(owner);
        return matches;
    };
    const auto expose_map = [&]() {
        const auto *queued = server.next_event(owner);
        const auto *event = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::ExposeEvent>(queued);
        const bool matches = event != nullptr && event->window == child &&
            event->x == 0 && event->y == 0 && event->width == 20 &&
            event->height == 15 && event->count == 0 &&
            event->sequence == 107;
        server.pop_event(owner);
        return matches;
    };
    if (!expect(server.set_window_mapped(*server.window(child), true) ==
                    xmin::server::EventDelivery::delivered &&
                    server.window(child)->mapped && structure_map() &&
                    visibility_map() &&
                    expose_map() &&
                    substructure_map() &&
                    !server.has_pending_event(owner) &&
                    !server.has_pending_event(observer),
                "MapNotify routing or payload is wrong")) {
        return false;
    }

    const auto structure_unmap = [&]() {
        const auto *queued = server.next_event(owner);
        const auto *event = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::UnmapNotifyEvent>(queued);
        const bool matches = event != nullptr && event->event == child &&
            event->window == child && !event->from_configure &&
            event->sequence == 107;
        server.pop_event(owner);
        return matches;
    };
    const auto substructure_unmap = [&]() {
        const auto *queued = server.next_event(observer);
        const auto *event = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::UnmapNotifyEvent>(queued);
        const bool matches = event != nullptr &&
            event->event == xmin::server::root_window_id &&
            event->window == child && !event->from_configure &&
            event->sequence == 109;
        server.pop_event(observer);
        return matches;
    };
    return expect(
        server.set_window_mapped(*server.window(child), false) ==
                xmin::server::EventDelivery::delivered &&
            !server.window(child)->mapped && structure_unmap() &&
            substructure_unmap() && !server.has_pending_event(owner) &&
            !server.has_pending_event(observer),
        "UnmapNotify routing or payload is wrong");
}

bool
test_window_manager_redirects()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t manager = 0x00400000;
    constexpr std::uint32_t child = owner | 1U;
    constexpr std::uint32_t sibling = owner | 2U;
    constexpr std::uint32_t structure_notify_mask = 1U << 17;
    constexpr std::uint32_t substructure_notify_mask = 1U << 19;
    constexpr std::uint32_t substructure_redirect_mask = 1U << 20;
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(owner) &&
                    server.register_client(manager),
                "window-manager redirect client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 41);
    server.note_client_sequence(manager, 73);

    xmin::server::WindowRecord candidate;
    candidate.id = child;
    candidate.parent = xmin::server::root_window_id;
    candidate.x = 3;
    candidate.y = 4;
    candidate.width = 30;
    candidate.height = 20;
    xmin::server::WindowRecord other = candidate;
    other.id = sibling;
    other.x = 40;
    if (!expect(server.add_window(std::move(candidate), owner) &&
                    server.add_window(std::move(other), owner),
                "window-manager redirect window insertion failed")) {
        return false;
    }
    auto *root = server.window(xmin::server::root_window_id);
    auto *stored = server.window(child);
    root->event_masks.emplace(
        manager, substructure_redirect_mask | substructure_notify_mask);
    stored->event_masks.emplace(owner, structure_notify_mask);

    if (!expect(server.redirect_map_request(owner, *stored) ==
                    xmin::server::RedirectDelivery::redirected &&
                    !stored->mapped,
                "MapWindow was not redirected")) {
        return false;
    }
    const auto *queued = server.next_event(manager);
    const auto *map = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::MapRequestEvent>(queued);
    if (!expect(map != nullptr &&
                    map->parent == xmin::server::root_window_id &&
                    map->window == child && map->sequence == 73,
                "MapRequest payload is wrong")) {
        return false;
    }
    server.pop_event(manager);

    if (!expect(server.redirect_configure_request(
                    owner, *stored, -2, 6, 44, 25, 2, 0x006f,
                    sibling, 1) ==
                    xmin::server::RedirectDelivery::redirected,
                "ConfigureWindow was not redirected")) {
        return false;
    }
    queued = server.next_event(manager);
    const auto *configure_request = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::ConfigureRequestEvent>(queued);
    if (!expect(configure_request != nullptr &&
                    configure_request->parent ==
                        xmin::server::root_window_id &&
                    configure_request->window == child &&
                    configure_request->sibling == sibling &&
                    configure_request->x == -2 &&
                    configure_request->y == 6 &&
                    configure_request->width == 44 &&
                    configure_request->height == 25 &&
                    configure_request->border_width == 2 &&
                    configure_request->stack_mode == 1 &&
                    configure_request->value_mask == 0x006f &&
                    configure_request->sequence == 73,
                "ConfigureRequest payload is wrong")) {
        return false;
    }
    server.pop_event(manager);

    if (!expect(server.redirect_map_request(manager, *stored) ==
                    xmin::server::RedirectDelivery::not_redirected,
                "a manager redirected its own MapWindow request") ||
        !expect(server.set_window_mapped(*stored, true) !=
                    xmin::server::EventDelivery::queue_full &&
                    server.set_window_mapped(*server.window(sibling), true) !=
                    xmin::server::EventDelivery::queue_full,
                "window-manager redirect mapping setup failed")) {
        return false;
    }
    // Discard the structure and substructure MapNotify events.
    server.pop_event(owner);
    server.pop_event(manager);
    server.pop_event(manager);

    if (!expect(server.redirect_circulate_request(owner, *root, true) ==
                    xmin::server::RedirectDelivery::redirected,
                "CirculateWindow was not redirected")) {
        return false;
    }
    queued = server.next_event(manager);
    const auto *circulate = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CirculateRequestEvent>(queued);
    if (!expect(circulate != nullptr &&
                    circulate->parent == xmin::server::root_window_id &&
                    circulate->window == child && circulate->place == 0 &&
                    circulate->sequence == 73,
                "CirculateRequest payload is wrong")) {
        return false;
    }
    server.pop_event(manager);

    if (!expect(server.configure_window(
                    *stored, 7, 8, 35, 22, 1,
                    std::nullopt, std::nullopt) ==
                    xmin::server::EventDelivery::delivered,
                "manager ConfigureWindow failed")) {
        return false;
    }
    queued = server.next_event(owner);
    const auto *structure = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::ConfigureNotifyEvent>(queued);
    if (!expect(structure != nullptr && structure->event == child &&
                    structure->window == child && structure->x == 7 &&
                    structure->y == 8 && structure->width == 35 &&
                    structure->height == 22 &&
                    structure->border_width == 1 &&
                    structure->sequence == 41,
                "structure ConfigureNotify payload is wrong")) {
        return false;
    }
    server.pop_event(owner);
    queued = server.next_event(manager);
    const auto *substructure = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::ConfigureNotifyEvent>(queued);
    if (!expect(substructure != nullptr &&
                    substructure->event == xmin::server::root_window_id &&
                    substructure->window == child &&
                    substructure->sequence == 73,
                "substructure ConfigureNotify payload is wrong")) {
        return false;
    }
    server.pop_event(manager);

    stored->event_masks.emplace(manager, 1U << 18);
    if (!expect(server.configure_window(
                    *stored, 9, 10, 60, 40, 1,
                    std::nullopt, std::nullopt, owner, 0x000f) ==
                    xmin::server::EventDelivery::delivered &&
                    stored->x == 9 && stored->y == 10 &&
                    stored->width == 35 && stored->height == 22,
                "ResizeRedirect did not preserve the managed size")) {
        return false;
    }
    queued = server.next_event(manager);
    const auto *resize = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::ResizeRequestEvent>(queued);
    if (!expect(resize != nullptr && resize->window == child &&
                    resize->width == 60 && resize->height == 40 &&
                    resize->sequence == 73,
                "ResizeRequest payload is wrong")) {
        return false;
    }
    server.pop_event(manager);
    // The effective ConfigureNotify follows the redirected resize request.
    queued = server.next_event(manager);
    substructure = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::ConfigureNotifyEvent>(queued);
    if (!expect(substructure != nullptr &&
                    substructure->width == 35 &&
                    substructure->height == 22,
                "ResizeRedirect ConfigureNotify reported the requested size")) {
        return false;
    }
    server.pop_event(manager);
    queued = server.next_event(owner);
    structure = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::ConfigureNotifyEvent>(queued);
    if (!expect(structure != nullptr && structure->x == 9 &&
                    structure->y == 10 && structure->width == 35 &&
                    structure->height == 22,
                "ResizeRedirect structure notification is wrong")) {
        return false;
    }
    server.pop_event(owner);

    if (!expect(server.circulate_window(
                    xmin::server::root_window_id, true) ==
                    xmin::server::EventDelivery::delivered,
                "manager CirculateWindow failed")) {
        return false;
    }
    queued = server.next_event(owner);
    const auto *circulate_notify = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CirculateNotifyEvent>(queued);
    if (!expect(circulate_notify != nullptr &&
                    circulate_notify->event == child &&
                    circulate_notify->window == child &&
                    circulate_notify->place == 0,
                "structure CirculateNotify payload is wrong")) {
        return false;
    }
    server.pop_event(owner);
    queued = server.next_event(manager);
    circulate_notify = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::CirculateNotifyEvent>(queued);
    if (!expect(circulate_notify != nullptr &&
                    circulate_notify->event ==
                        xmin::server::root_window_id &&
                    circulate_notify->window == child &&
                    circulate_notify->place == 0,
                "substructure CirculateNotify payload is wrong")) {
        return false;
    }
    server.pop_event(manager);

    stored->override_redirect = true;
    return expect(server.redirect_map_request(owner, *stored) ==
                      xmin::server::RedirectDelivery::not_redirected,
                  "override-redirect MapWindow was redirected") &&
        expect(!server.has_pending_event(owner) &&
                   !server.has_pending_event(manager),
               "window-manager redirect test left queued events");
}

bool
test_resize_exposures()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t window_id = owner | 1U;
    constexpr std::uint32_t exposure_mask = 1U << 15;
    constexpr std::uint32_t background = 0x00112233U;
    constexpr std::uint32_t foreground = 0x00abcdefU;
    xmin::server::ServerState server(32, 24);
    if (!expect(server.register_client(owner),
                "resize-exposure client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 117);
    auto surface = xmin::server::Surface::create(4, 3, 24);
    if (!expect(surface.has_value(),
                "resize-exposure surface allocation failed")) {
        return false;
    }
    surface->fill({0, 0, 4, 3}, foreground, 3, 0xffffffffU);
    xmin::server::WindowRecord window;
    window.id = window_id;
    window.parent = xmin::server::root_window_id;
    window.width = 4;
    window.height = 3;
    window.mapped = true;
    window.background_pixel = background;
    window.surface = server.adopt_surface(std::move(*surface));
    window.event_masks.emplace(owner, exposure_mask);
    if (!expect(window.surface != nullptr &&
                    server.add_window(std::move(window), owner),
                "resize-exposure window insertion failed")) {
        return false;
    }

    auto *stored = server.window(window_id);
    if (!expect(server.configure_window(
                    *stored, 0, 0, 6, 5, 0,
                    std::nullopt, std::nullopt) ==
                    xmin::server::EventDelivery::delivered &&
                    stored->surface->pixel(0, 0) == background &&
                    stored->surface->pixel(5, 4) == background,
                "ForgetGravity did not clear a resized window")) {
        return false;
    }
    const auto *queued = server.next_event(owner);
    const auto *expose = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::ExposeEvent>(queued);
    if (!expect(expose != nullptr && expose->window == window_id &&
                    expose->x == 0 && expose->y == 0 &&
                    expose->width == 6 && expose->height == 5 &&
                    expose->count == 0 && expose->sequence == 117,
                "ForgetGravity resize exposure is malformed")) {
        return false;
    }
    server.pop_event(owner);

    stored->bit_gravity = 1; // NorthWestGravity
    stored->surface->fill({0, 0, 6, 5}, foreground, 3, 0xffffffffU);
    if (!expect(server.configure_window(
                    *stored, 0, 0, 8, 6, 0,
                    std::nullopt, std::nullopt) ==
                    xmin::server::EventDelivery::delivered &&
                    stored->surface->pixel(0, 0) == foreground &&
                    stored->surface->pixel(5, 4) == foreground &&
                    stored->surface->pixel(7, 0) == background &&
                    stored->surface->pixel(0, 5) == background,
                "NorthWestGravity did not preserve resized contents")) {
        return false;
    }
    queued = server.next_event(owner);
    expose = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::ExposeEvent>(queued);
    const bool bottom = expose != nullptr && expose->x == 0 &&
        expose->y == 5 && expose->width == 8 && expose->height == 1 &&
        expose->count == 1;
    server.pop_event(owner);
    queued = server.next_event(owner);
    expose = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::ExposeEvent>(queued);
    const bool right = expose != nullptr && expose->x == 6 &&
        expose->y == 0 && expose->width == 2 && expose->height == 5 &&
        expose->count == 0;
    server.pop_event(owner);
    return expect(bottom && right && !server.has_pending_event(owner),
                  "NorthWestGravity resize regions are malformed");
}

bool
test_mapping_lifecycle_events()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t parent = owner | 1U;
    constexpr std::uint32_t child = owner | 2U;
    constexpr std::uint32_t sibling = owner | 3U;
    constexpr std::uint32_t crossing_mask = (1U << 4) | (1U << 5);
    constexpr std::uint32_t focus_mask = 1U << 21;
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(owner),
                "lifecycle client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 97);
    const auto add_window = [&](std::uint32_t id, std::uint32_t parent_id,
                                std::int16_t x, std::int16_t y,
                                std::uint16_t width,
                                std::uint16_t height) {
        xmin::server::WindowRecord candidate;
        candidate.id = id;
        candidate.parent = parent_id;
        candidate.x = x;
        candidate.y = y;
        candidate.width = width;
        candidate.height = height;
        if (!server.add_window(std::move(candidate), owner))
            return false;
        return server.set_window_mapped(*server.window(id), true) !=
            xmin::server::EventDelivery::queue_full;
    };
    if (!expect(add_window(parent, xmin::server::root_window_id,
                           5, 5, 50, 50) &&
                    add_window(child, parent, 5, 5, 20, 20) &&
                    server.inject_input(6, 0, 15, 15) ==
                        xmin::server::EventDelivery::no_recipient &&
                    server.set_input_focus(
                        xmin::server::FocusKind::window, child, 2, 0) ==
                        xmin::server::FocusUpdate::updated,
                "lifecycle window setup failed")) {
        return false;
    }
    server.window(parent)->event_masks.emplace(
        owner, crossing_mask | focus_mask);
    server.window(child)->event_masks.emplace(
        owner, crossing_mask | focus_mask);

    const auto focus = [&](std::uint8_t type, std::uint8_t detail,
                           std::uint32_t event) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::FocusEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == event &&
            value->mode == 0 && value->sequence == 97;
        server.pop_event(owner);
        return matches;
    };
    const auto crossing = [&](std::uint8_t type, std::uint8_t detail,
                              std::uint32_t event,
                              std::int16_t event_x,
                              std::int16_t event_y) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::CrossingEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == event &&
            value->child == 0 && value->event_x == event_x &&
            value->event_y == event_y && value->mode == 0 &&
            value->same_screen && value->focus && value->sequence == 97;
        server.pop_event(owner);
        return matches;
    };
    if (!expect(server.set_window_mapped(*server.window(child), false) ==
                    xmin::server::EventDelivery::delivered &&
                    !server.window(child)->mapped &&
                    server.input().focus.kind ==
                        xmin::server::FocusKind::window &&
                    server.input().focus.window == parent &&
                    server.input().focus.revert_to == 0 &&
                    focus(10, 0, child) && focus(9, 2, parent) &&
                    crossing(8, 0, child, 5, 5) &&
                    crossing(7, 2, parent, 10, 10) &&
                    !server.has_pending_event(owner),
                "unmap lifecycle transition is wrong")) {
        return false;
    }
    if (!expect(server.set_window_mapped(*server.window(child), true) ==
                    xmin::server::EventDelivery::delivered &&
                    server.window(child)->mapped &&
                    server.input().focus.window == parent &&
                    crossing(8, 2, parent, 10, 10) &&
                    crossing(7, 0, child, 5, 5) &&
                    !server.has_pending_event(owner),
                "map lifecycle transition is wrong")) {
        return false;
    }

    xmin::server::WindowRecord sibling_window;
    sibling_window.id = sibling;
    sibling_window.parent = parent;
    sibling_window.x = 30;
    sibling_window.y = 30;
    sibling_window.width = 10;
    sibling_window.height = 10;
    if (!expect(server.add_window(std::move(sibling_window), owner) &&
                    server.set_window_mapped(
                        *server.window(sibling), true) !=
                        xmin::server::EventDelivery::queue_full &&
                    server.set_input_focus(
                        xmin::server::FocusKind::window, child, 2, 0) ==
                        xmin::server::FocusUpdate::updated,
                "atomic lifecycle failure setup failed")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);

    xmin::server::ClientMessageEvent message;
    message.window = child;
    for (std::size_t count = 0;
         count + 3 < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(
                        child, 0, false, message) ==
                        xmin::server::EventDelivery::delivered,
                    "lifecycle queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.destroy_subwindows(parent) ==
                    xmin::server::EventDelivery::queue_full &&
                    server.window(child)->mapped &&
                    server.window(sibling)->mapped &&
                    server.input().focus.kind ==
                        xmin::server::FocusKind::window &&
                    server.input().focus.window == child &&
                    server.input().focus.revert_to == 2,
                "destruction escaped an atomic lifecycle queue failure")) {
        return false;
    }
    std::size_t queued_count = 0;
    while (server.has_pending_event(owner)) {
        const auto *queued = server.next_event(owner);
        if (!expect(queued != nullptr &&
                        std::holds_alternative<
                            xmin::server::ClientMessageEvent>(*queued),
                    "destroy failure left a partial lifecycle event")) {
            return false;
        }
        server.pop_event(owner);
        ++queued_count;
    }
    if (!expect(
            queued_count + 3 ==
                xmin::server::maximum_pending_events_per_client,
            "destroy failure changed the preexisting event count") ||
        !expect(server.destroy_window(child) ==
                    xmin::server::EventDelivery::delivered &&
                    server.window(child) == nullptr &&
                    server.input().focus.kind ==
                        xmin::server::FocusKind::window &&
                    server.input().focus.window == parent &&
                    focus(10, 0, child) && focus(9, 2, parent) &&
                    crossing(8, 0, child, 5, 5) &&
                    crossing(7, 2, parent, 10, 10) &&
                    !server.has_pending_event(owner),
                "destroy lifecycle transition is wrong")) {
        return false;
    }
    return expect(server.destroy_subwindows(parent) ==
                      xmin::server::EventDelivery::no_recipient &&
                      server.window(sibling) == nullptr &&
                      server.window(parent)->children.empty(),
                  "DestroySubwindows retained an unreachable child");
}

bool
test_reparent_lifecycle_events()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t parent = owner | 1U;
    constexpr std::uint32_t child = owner | 2U;
    constexpr std::uint32_t crossing_mask = (1U << 4) | (1U << 5);
    constexpr std::uint32_t focus_mask = 1U << 21;
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(owner),
                "reparent client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 101);
    xmin::server::WindowRecord parent_window;
    parent_window.id = parent;
    parent_window.parent = xmin::server::root_window_id;
    parent_window.x = 5;
    parent_window.y = 5;
    parent_window.width = 40;
    parent_window.height = 40;
    xmin::server::WindowRecord child_window;
    child_window.id = child;
    child_window.parent = xmin::server::root_window_id;
    child_window.x = 10;
    child_window.y = 10;
    child_window.width = 20;
    child_window.height = 20;
    if (!expect(server.add_window(std::move(parent_window), owner) &&
                    server.add_window(std::move(child_window), owner) &&
                    server.set_window_mapped(*server.window(parent), true) !=
                        xmin::server::EventDelivery::queue_full &&
                    server.set_window_mapped(*server.window(child), true) !=
                        xmin::server::EventDelivery::queue_full &&
                    server.inject_input(6, 0, 15, 15) ==
                        xmin::server::EventDelivery::no_recipient &&
                    server.set_input_focus(
                        xmin::server::FocusKind::window, child, 2, 0) ==
                        xmin::server::FocusUpdate::updated,
                "reparent lifecycle setup failed")) {
        return false;
    }
    server.window(xmin::server::root_window_id)->event_masks.emplace(
        owner, focus_mask);
    server.window(parent)->event_masks.emplace(
        owner, crossing_mask | focus_mask);
    server.window(child)->event_masks.emplace(
        owner, crossing_mask | focus_mask);

    const auto focus = [&](std::uint8_t type, std::uint8_t detail,
                           std::uint32_t event) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::FocusEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == event &&
            value->mode == 0 && value->sequence == 101;
        server.pop_event(owner);
        return matches;
    };
    const auto crossing = [&](std::uint8_t type, std::uint8_t detail,
                              std::uint32_t event,
                              std::int16_t event_x,
                              std::int16_t event_y) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::CrossingEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == event &&
            value->child == 0 && value->event_x == event_x &&
            value->event_y == event_y && value->mode == 0 &&
            value->same_screen && value->focus && value->sequence == 101;
        server.pop_event(owner);
        return matches;
    };
    if (!expect(server.reparent_window(child, parent, 5, 5) ==
                    xmin::server::ReparentUpdate::updated &&
                    server.window(child)->parent == parent &&
                    server.window(child)->x == 5 &&
                    server.window(child)->y == 5 &&
                    server.window(child)->mapped &&
                    server.input().focus.kind ==
                        xmin::server::FocusKind::window &&
                    server.input().focus.window ==
                        xmin::server::root_window_id &&
                    server.input().focus.revert_to == 0 &&
                    focus(10, 0, child) &&
                    focus(9, 2, xmin::server::root_window_id) &&
                    crossing(8, 3, child, 5, 5) &&
                    crossing(7, 3, parent, 10, 10) &&
                    crossing(8, 2, parent, 10, 10) &&
                    crossing(7, 0, child, 5, 5) &&
                    !server.has_pending_event(owner),
                "reparent lifecycle sequence is wrong")) {
        return false;
    }

    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window, child, 2, 0) ==
                    xmin::server::FocusUpdate::updated,
                "reparent rollback focus setup failed")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);
    xmin::server::ClientMessageEvent message;
    message.window = child;
    for (std::size_t count = 0;
         count + 1 < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(
                        child, 0, false, message) ==
                        xmin::server::EventDelivery::delivered,
                    "reparent queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.reparent_window(
                    child, xmin::server::root_window_id, 10, 10) ==
                    xmin::server::ReparentUpdate::queue_full &&
                    server.window(child)->parent == parent &&
                    server.window(child)->x == 5 &&
                    server.window(child)->y == 5 &&
                    server.window(child)->mapped &&
                    server.window(parent)->children.size() == 1 &&
                    server.window(parent)->children.front() == child &&
                    server.window(xmin::server::root_window_id)
                            ->children.size() == 1 &&
                    server.window(xmin::server::root_window_id)
                            ->children.front() == parent &&
                    server.input().focus.kind ==
                        xmin::server::FocusKind::window &&
                    server.input().focus.window == child &&
                    server.input().focus.revert_to == 2,
                "reparent queue failure leaked tree or focus state")) {
        return false;
    }
    std::size_t queued_count = 0;
    while (server.has_pending_event(owner)) {
        const auto *queued = server.next_event(owner);
        if (!expect(queued != nullptr &&
                        std::holds_alternative<
                            xmin::server::ClientMessageEvent>(*queued),
                    "reparent queue failure left a partial event path")) {
            return false;
        }
        server.pop_event(owner);
        ++queued_count;
    }
    return expect(
        queued_count + 1 == xmin::server::maximum_pending_events_per_client,
        "reparent queue failure changed the preexisting event count");
}

bool
test_grab_transitions()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t parent = owner | 1U;
    constexpr std::uint32_t child = owner | 2U;
    constexpr std::uint32_t crossing_mask = (1U << 4) | (1U << 5);
    constexpr std::uint32_t focus_mask = 1U << 21;
    constexpr std::uint32_t key_mask = (1U << 0) | (1U << 1);
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(owner),
                "grab-transition client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 103);
    xmin::server::WindowRecord parent_window;
    parent_window.id = parent;
    parent_window.parent = xmin::server::root_window_id;
    parent_window.x = 5;
    parent_window.y = 5;
    parent_window.width = 40;
    parent_window.height = 40;
    xmin::server::WindowRecord child_window;
    child_window.id = child;
    child_window.parent = parent;
    child_window.x = 5;
    child_window.y = 5;
    child_window.width = 20;
    child_window.height = 20;
    if (!expect(server.add_window(std::move(parent_window), owner) &&
                    server.add_window(std::move(child_window), owner) &&
                    server.set_window_mapped(*server.window(parent), true) !=
                        xmin::server::EventDelivery::queue_full &&
                    server.set_window_mapped(*server.window(child), true) !=
                        xmin::server::EventDelivery::queue_full &&
                    server.inject_input(6, 0, 15, 15) ==
                        xmin::server::EventDelivery::no_recipient &&
                    server.set_input_focus(
                        xmin::server::FocusKind::window,
                        xmin::server::root_window_id, 0, 0) ==
                        xmin::server::FocusUpdate::updated,
                "grab-transition setup failed")) {
        return false;
    }
    server.window(xmin::server::root_window_id)->event_masks.emplace(
        owner, focus_mask);
    server.window(parent)->event_masks.emplace(
        owner, crossing_mask | focus_mask);
    server.window(child)->event_masks.emplace(
        owner, crossing_mask | focus_mask | key_mask);

    const auto crossing = [&](std::uint8_t type, std::uint8_t detail,
                              std::uint8_t mode, std::uint32_t event) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::CrossingEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == event &&
            value->mode == mode && value->sequence == 103;
        server.pop_event(owner);
        return matches;
    };
    const auto focus = [&](std::uint8_t type, std::uint8_t detail,
                           std::uint8_t mode, std::uint32_t event) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::FocusEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == event &&
            value->mode == mode && value->sequence == 103;
        server.pop_event(owner);
        return matches;
    };
    const auto key = [&](std::uint8_t type, std::uint8_t detail) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::CoreInputEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == child &&
            value->sequence == 103;
        server.pop_event(owner);
        return matches;
    };

    if (!expect(server.activate_pointer_grab(xmin::server::ActiveGrab{
                    owner, parent, 0, server.current_time(), crossing_mask,
                    1, 1, false}) ==
                    xmin::server::EventDelivery::delivered &&
                    server.input().pointer_grab &&
                    crossing(8, 0, 1, child) &&
                    crossing(7, 2, 1, parent) &&
                    server.deactivate_pointer_grab() ==
                        xmin::server::EventDelivery::delivered &&
                    !server.input().pointer_grab &&
                    crossing(8, 2, 2, parent) &&
                    crossing(7, 0, 2, child) &&
                    !server.has_pending_event(owner),
                "explicit pointer grab transition is wrong")) {
        return false;
    }

    if (!expect(server.activate_keyboard_grab(xmin::server::ActiveGrab{
                    owner, child, 0, server.current_time(), key_mask,
                    1, 1, false}) ==
                    xmin::server::EventDelivery::delivered &&
                    server.input().keyboard_grab &&
                    focus(10, 5, 1, child) &&
                    focus(10, 5, 1, parent) &&
                    focus(10, 2, 1, xmin::server::root_window_id) &&
                    focus(9, 1, 1, parent) &&
                    focus(9, 0, 1, child) &&
                    server.deactivate_keyboard_grab() ==
                        xmin::server::EventDelivery::delivered &&
                    !server.input().keyboard_grab &&
                    focus(10, 0, 2, child) &&
                    focus(10, 1, 2, parent) &&
                    focus(9, 2, 2, xmin::server::root_window_id) &&
                    !server.has_pending_event(owner),
                "explicit keyboard grab transition is wrong")) {
        return false;
    }

    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window, child, 0, 0) ==
                    xmin::server::FocusUpdate::updated,
                "same-focus grab setup failed")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);
    if (!expect(server.activate_keyboard_grab(xmin::server::ActiveGrab{
                    owner, child, 0, server.current_time(), key_mask,
                    1, 1, false}) ==
                    xmin::server::EventDelivery::delivered &&
                    focus(10, 3, 1, child) && focus(9, 3, 1, child) &&
                    server.deactivate_keyboard_grab() ==
                        xmin::server::EventDelivery::delivered &&
                    focus(10, 3, 2, child) && focus(9, 3, 2, child) &&
                    !server.has_pending_event(owner),
                "same-focus explicit keyboard transition is wrong")) {
        return false;
    }

    xmin::server::PassiveGrab passive;
    passive.kind = xmin::server::PassiveGrabKind::key;
    passive.details = xmin::server::passive_grab_details(passive.kind, 40);
    passive.modifiers = xmin::server::passive_grab_modifiers(0);
    passive.owner = owner;
    passive.window = child;
    passive.event_mask = key_mask;
    if (!expect(server.add_passive_grab(std::move(passive)) ==
                    xmin::server::PassiveGrabUpdate::updated &&
                    server.inject_input(2, 40, 15, 15) ==
                        xmin::server::EventDelivery::delivered &&
                    server.input().keyboard_grab &&
                    server.input().keyboard_grab->passive &&
                    focus(10, 3, 1, child) && focus(9, 3, 1, child) &&
                    key(2, 40) &&
                    server.inject_input(3, 40, 15, 15) ==
                        xmin::server::EventDelivery::delivered &&
                    !server.input().keyboard_grab && key(3, 40) &&
                    focus(10, 3, 2, child) && focus(9, 3, 2, child) &&
                    !server.has_pending_event(owner),
                "passive keyboard grab transition is wrong")) {
        return false;
    }

    xmin::server::ClientMessageEvent message;
    message.window = child;
    for (std::size_t count = 0;
         count + 1 < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(
                        child, 0, false, message) ==
                        xmin::server::EventDelivery::delivered,
                    "grab-transition queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.activate_pointer_grab(xmin::server::ActiveGrab{
                    owner, parent, 0, server.current_time(), crossing_mask,
                    1, 1, false}) ==
                    xmin::server::EventDelivery::queue_full &&
                    !server.input().pointer_grab,
                "queue failure committed an explicit pointer grab")) {
        return false;
    }
    std::size_t queued_count = 0;
    while (server.has_pending_event(owner)) {
        const auto *queued = server.next_event(owner);
        if (!expect(queued != nullptr &&
                        std::holds_alternative<
                            xmin::server::ClientMessageEvent>(*queued),
                    "grab activation failure left a partial crossing")) {
            return false;
        }
        server.pop_event(owner);
        ++queued_count;
    }
    return expect(
        queued_count + 1 == xmin::server::maximum_pending_events_per_client,
        "grab activation failure changed the existing event count");
}

bool
test_disconnect_grab_transitions()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t observer = 0x00400000;
    constexpr std::uint32_t child = owner | 1U;
    constexpr std::uint32_t crossing_mask = (1U << 4) | (1U << 5);
    constexpr std::uint32_t focus_mask = 1U << 21;
    const auto setup = [&](xmin::server::ServerState &server) {
        if (!server.register_client(owner) ||
            !server.register_client(observer)) {
            return false;
        }
        server.note_client_sequence(observer, 113);
        xmin::server::WindowRecord window;
        window.id = child;
        window.parent = xmin::server::root_window_id;
        window.x = 5;
        window.y = 5;
        window.width = 30;
        window.height = 30;
        if (!server.add_window(std::move(window), owner) ||
            server.set_window_mapped(*server.window(child), true) ==
                xmin::server::EventDelivery::queue_full ||
            server.inject_input(6, 0, 15, 15) !=
                xmin::server::EventDelivery::no_recipient ||
            server.set_input_focus(
                xmin::server::FocusKind::window,
                xmin::server::root_window_id, 0, 0) !=
                xmin::server::FocusUpdate::updated) {
            return false;
        }
        server.window(xmin::server::root_window_id)->event_masks.emplace(
            observer, crossing_mask | focus_mask);
        server.window(child)->event_masks.emplace(
            observer, crossing_mask | focus_mask);
        server.input().pointer_grab = xmin::server::ActiveGrab{
            owner, xmin::server::root_window_id, 0,
            server.current_time(), crossing_mask, 1, 1, false};
        server.input().keyboard_grab = xmin::server::ActiveGrab{
            owner, child, 0, server.current_time(), 3, 1, 1, false};
        return true;
    };

    xmin::server::ServerState server(100, 80);
    if (!expect(setup(server), "disconnect-grab setup failed"))
        return false;
    server.disconnect_client(owner);
    const auto crossing = [&](std::uint8_t type, std::uint8_t detail,
                              std::uint8_t mode, std::uint32_t event) {
        const auto *queued = server.next_event(observer);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::CrossingEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->mode == mode &&
            value->event == event && value->sequence == 113;
        server.pop_event(observer);
        return matches;
    };
    const auto focus = [&](std::uint8_t type, std::uint8_t detail,
                           std::uint8_t mode, std::uint32_t event) {
        const auto *queued = server.next_event(observer);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::FocusEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->mode == mode &&
            value->event == event && value->sequence == 113;
        server.pop_event(observer);
        return matches;
    };
    if (!expect(!server.input().pointer_grab &&
                    !server.input().keyboard_grab &&
                    server.window(child) == nullptr &&
                    crossing(8, 2, 2, xmin::server::root_window_id) &&
                    crossing(7, 0, 2, child) &&
                    focus(10, 0, 2, child) &&
                    focus(9, 2, 2, xmin::server::root_window_id) &&
                    crossing(8, 0, 0, child) &&
                    crossing(7, 2, 0, xmin::server::root_window_id) &&
                    !server.has_pending_event(observer),
                "disconnect did not order ungrabs before teardown")) {
        return false;
    }

    xmin::server::ServerState pressured(100, 80);
    if (!expect(setup(pressured),
                "disconnect queue-pressure setup failed")) {
        return false;
    }
    xmin::server::ClientMessageEvent message;
    message.window = child;
    for (std::size_t count = 0;
         count < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(pressured.deliver_client_message(
                        child, crossing_mask, false, message) ==
                        xmin::server::EventDelivery::delivered,
                    "disconnect observer queue fill failed")) {
            return false;
        }
    }
    pressured.disconnect_client(owner);
    if (!expect(!pressured.input().pointer_grab &&
                    !pressured.input().keyboard_grab &&
                    pressured.window(child) == nullptr,
                "queue pressure retained dead-client state")) {
        return false;
    }
    std::size_t queued_count = 0;
    while (pressured.has_pending_event(observer)) {
        const auto *queued = pressured.next_event(observer);
        if (!expect(queued != nullptr &&
                        std::holds_alternative<
                            xmin::server::ClientMessageEvent>(*queued),
                    "queue pressure left a partial disconnect transition")) {
            return false;
        }
        pressured.pop_event(observer);
        ++queued_count;
    }
    return expect(
        queued_count == xmin::server::maximum_pending_events_per_client,
        "disconnect changed the observer's full queue");
}

bool
test_pointer_grab_view_loss()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t parent = owner | 1U;
    constexpr std::uint32_t child = owner | 2U;
    constexpr std::uint32_t crossing_mask = (1U << 4) | (1U << 5);
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(owner),
                "view-loss client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 107);
    xmin::server::WindowRecord parent_window;
    parent_window.id = parent;
    parent_window.parent = xmin::server::root_window_id;
    parent_window.x = 5;
    parent_window.y = 5;
    parent_window.width = 40;
    parent_window.height = 40;
    xmin::server::WindowRecord child_window;
    child_window.id = child;
    child_window.parent = parent;
    child_window.x = 5;
    child_window.y = 5;
    child_window.width = 20;
    child_window.height = 20;
    if (!expect(server.add_window(std::move(parent_window), owner) &&
                    server.add_window(std::move(child_window), owner) &&
                    server.set_window_mapped(*server.window(parent), true) !=
                        xmin::server::EventDelivery::queue_full &&
                    server.set_window_mapped(*server.window(child), true) !=
                        xmin::server::EventDelivery::queue_full &&
                    server.inject_input(6, 0, 15, 15) ==
                        xmin::server::EventDelivery::no_recipient &&
                    server.set_input_focus(
                        xmin::server::FocusKind::window, parent, 0, 0) ==
                        xmin::server::FocusUpdate::updated,
                "view-loss window setup failed")) {
        return false;
    }
    server.window(parent)->event_masks.emplace(owner, crossing_mask);
    server.window(child)->event_masks.emplace(owner, crossing_mask);
    const auto arm_grab = [&] {
        server.input().pointer_grab = xmin::server::ActiveGrab{
            owner, parent, child, server.current_time(), crossing_mask,
            1, 1, false};
    };
    arm_grab();

    const auto crossing = [&](std::uint8_t type, std::uint8_t detail,
                              std::uint8_t mode, std::uint32_t event,
                              std::int16_t event_x,
                              std::int16_t event_y) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::CrossingEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == event &&
            value->child == 0 && value->event_x == event_x &&
            value->event_y == event_y && value->mode == mode &&
            value->same_screen && value->focus && value->sequence == 107;
        server.pop_event(owner);
        return matches;
    };
    if (!expect(server.set_window_mapped(*server.window(child), false) ==
                    xmin::server::EventDelivery::delivered &&
                    !server.input().pointer_grab &&
                    !server.window(child)->mapped &&
                    crossing(8, 2, 2, parent, 10, 10) &&
                    crossing(7, 0, 2, child, 5, 5) &&
                    crossing(8, 0, 0, child, 5, 5) &&
                    crossing(7, 2, 0, parent, 10, 10) &&
                    !server.has_pending_event(owner),
                "pointer-grab view-loss sequence is wrong")) {
        return false;
    }
    if (!expect(server.set_window_mapped(*server.window(child), true) ==
                    xmin::server::EventDelivery::delivered,
                "view-loss rollback remap failed")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);
    arm_grab();
    xmin::server::ClientMessageEvent message;
    message.window = child;
    for (std::size_t count = 0;
         count + 3 < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(
                        child, 0, false, message) ==
                        xmin::server::EventDelivery::delivered,
                    "view-loss queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.set_window_mapped(*server.window(child), false) ==
                    xmin::server::EventDelivery::queue_full &&
                    server.window(child)->mapped &&
                    server.input().pointer_grab &&
                    server.input().pointer_grab->window == parent &&
                    server.input().pointer_grab->confine_to == child,
                "queue failure released a live pointer grab")) {
        return false;
    }
    std::size_t queued_count = 0;
    while (server.has_pending_event(owner)) {
        const auto *queued = server.next_event(owner);
        if (!expect(queued != nullptr &&
                        std::holds_alternative<
                            xmin::server::ClientMessageEvent>(*queued),
                    "view-loss queue failure left a partial crossing")) {
            return false;
        }
        server.pop_event(owner);
        ++queued_count;
    }
    return expect(
        queued_count + 3 == xmin::server::maximum_pending_events_per_client,
        "view-loss queue failure changed the existing event count");
}

bool
test_keyboard_grab_view_loss()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t parent = owner | 1U;
    constexpr std::uint32_t child = owner | 2U;
    constexpr std::uint32_t crossing_mask = (1U << 4) | (1U << 5);
    constexpr std::uint32_t focus_mask = 1U << 21;
    xmin::server::ServerState server(100, 80);
    if (!expect(server.register_client(owner),
                "keyboard view-loss client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 109);
    xmin::server::WindowRecord parent_window;
    parent_window.id = parent;
    parent_window.parent = xmin::server::root_window_id;
    parent_window.x = 5;
    parent_window.y = 5;
    parent_window.width = 40;
    parent_window.height = 40;
    xmin::server::WindowRecord child_window;
    child_window.id = child;
    child_window.parent = parent;
    child_window.x = 5;
    child_window.y = 5;
    child_window.width = 20;
    child_window.height = 20;
    if (!expect(server.add_window(std::move(parent_window), owner) &&
                    server.add_window(std::move(child_window), owner) &&
                    server.set_window_mapped(*server.window(parent), true) !=
                        xmin::server::EventDelivery::queue_full &&
                    server.set_window_mapped(*server.window(child), true) !=
                        xmin::server::EventDelivery::queue_full &&
                    server.inject_input(6, 0, 15, 15) ==
                        xmin::server::EventDelivery::no_recipient &&
                    server.set_input_focus(
                        xmin::server::FocusKind::window,
                        xmin::server::root_window_id, 0, 0) ==
                        xmin::server::FocusUpdate::updated,
                "keyboard view-loss setup failed")) {
        return false;
    }
    server.window(xmin::server::root_window_id)->event_masks.emplace(
        owner, focus_mask);
    server.window(parent)->event_masks.emplace(
        owner, crossing_mask | focus_mask);
    server.window(child)->event_masks.emplace(
        owner, crossing_mask | focus_mask);
    const auto arm_grab = [&] {
        server.input().keyboard_grab = xmin::server::ActiveGrab{
            owner, child, 0, server.current_time(), 0, 1, 1, false};
    };
    arm_grab();

    const auto focus = [&](std::uint8_t type, std::uint8_t detail,
                           std::uint8_t mode, std::uint32_t event) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::FocusEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == event &&
            value->mode == mode && value->sequence == 109;
        if (!matches) {
            if (queued == nullptr) {
                std::cerr << "keyboard view-loss focus queue is empty\n";
            }
            else if (value == nullptr) {
                std::cerr << "keyboard view-loss expected focus, got "
                          << (std::holds_alternative<
                                  xmin::server::CrossingEvent>(*queued)
                                  ? "crossing"
                                  : "other")
                          << '\n';
            }
            else {
                std::cerr << "keyboard view-loss focus mismatch: type="
                          << unsigned(value->type) << " detail="
                          << unsigned(value->detail) << " mode="
                          << unsigned(value->mode) << " event="
                          << value->event << '\n';
            }
        }
        server.pop_event(owner);
        return matches;
    };
    const auto crossing = [&](std::uint8_t type, std::uint8_t detail,
                              std::uint32_t event,
                              std::int16_t event_x,
                              std::int16_t event_y) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::server::CrossingEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == event &&
            value->child == 0 && value->event_x == event_x &&
            value->event_y == event_y && value->mode == 0 &&
            value->same_screen && value->focus && value->sequence == 109;
        if (!matches) {
            if (value == nullptr) {
                std::cerr << "keyboard view-loss expected crossing event\n";
            }
            else {
                std::cerr << "keyboard view-loss crossing mismatch: type="
                          << unsigned(value->type) << " detail="
                          << unsigned(value->detail) << " mode="
                          << unsigned(value->mode) << " event="
                          << value->event << '\n';
            }
        }
        server.pop_event(owner);
        return matches;
    };
    if (!expect(server.set_window_mapped(*server.window(child), false) ==
                    xmin::server::EventDelivery::delivered &&
                    !server.input().keyboard_grab &&
                    !server.window(child)->mapped &&
                    server.input().focus.window ==
                        xmin::server::root_window_id &&
                    focus(10, 0, 2, child) &&
                    focus(10, 1, 2, parent) &&
                    focus(9, 2, 2, xmin::server::root_window_id) &&
                    crossing(8, 0, child, 5, 5) &&
                    crossing(7, 2, parent, 10, 10) &&
                    !server.has_pending_event(owner),
                "keyboard-grab view-loss sequence is wrong")) {
        return false;
    }
    if (!expect(server.set_window_mapped(*server.window(child), true) ==
                    xmin::server::EventDelivery::delivered,
                "keyboard view-loss remap failed")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);
    if (!expect(server.set_input_focus(
                    xmin::server::FocusKind::window, child, 2, 0) ==
                    xmin::server::FocusUpdate::updated,
                "same-focus keyboard view-loss setup failed")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);
    arm_grab();
    if (!expect(server.input().focus.window == child &&
                    server.input().focus.revert_to == 2 &&
                    server.input().keyboard_grab &&
                    server.input().keyboard_grab->window == child &&
                    (server.window(child)->event_masks.at(owner) &
                     focus_mask) != 0,
                "same-focus keyboard state was not armed") ||
        !expect(server.set_window_mapped(*server.window(child), false) ==
                    xmin::server::EventDelivery::delivered &&
                    !server.input().keyboard_grab &&
                    server.input().focus.window == parent &&
                    focus(10, 3, 2, child) &&
                    focus(9, 3, 2, child) &&
                    focus(10, 0, 0, child) &&
                    focus(9, 2, 0, parent) &&
                    crossing(8, 0, child, 5, 5) &&
                    crossing(7, 2, parent, 10, 10) &&
                    !server.has_pending_event(owner),
                "same-focus keyboard view-loss sequence is wrong") ||
        !expect(server.set_window_mapped(*server.window(child), true) ==
                    xmin::server::EventDelivery::delivered,
                "same-focus keyboard remap failed")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);
    arm_grab();
    xmin::server::ClientMessageEvent message;
    message.window = child;
    for (std::size_t count = 0;
         count + 3 < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(
                        child, 0, false, message) ==
                        xmin::server::EventDelivery::delivered,
                    "keyboard view-loss queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.set_window_mapped(*server.window(child), false) ==
                    xmin::server::EventDelivery::queue_full &&
                    server.window(child)->mapped &&
                    server.input().keyboard_grab &&
                    server.input().keyboard_grab->window == child,
                "queue failure released a live keyboard grab")) {
        return false;
    }
    std::size_t queued_count = 0;
    while (server.has_pending_event(owner)) {
        const auto *queued = server.next_event(owner);
        if (!expect(queued != nullptr &&
                        std::holds_alternative<
                            xmin::server::ClientMessageEvent>(*queued),
                    "keyboard view-loss failure left a partial event")) {
            return false;
        }
        server.pop_event(owner);
        ++queued_count;
    }
    return expect(
        queued_count + 3 == xmin::server::maximum_pending_events_per_client,
        "keyboard view-loss failure changed the existing event count");
}

bool
test_true_color()
{
    const auto red = xmin::server::parse_color("Red");
    const auto compact = xmin::server::parse_color("#0f8");
    const auto precise = xmin::server::parse_color("#123456789abc");
    return expect(red && red->red == 0xffff && red->green == 0 &&
                      red->blue == 0,
                  "named color parsing failed") &&
        expect(compact && compact->red == 0 && compact->green == 0xffff &&
                      compact->blue == 0x8888,
               "compact hexadecimal color parsing failed") &&
        expect(precise && precise->red == 0x1234 &&
                      precise->green == 0x5678 && precise->blue == 0x9abc,
               "precise hexadecimal color parsing failed") &&
        expect(!xmin::server::parse_color("not-a-color"),
               "invalid color name was accepted") &&
        expect(xmin::server::true_color_pixel(*red) == 0x00ff0000U,
               "TrueColor pixel packing failed") &&
        expect(xmin::server::true_color_rgb(0x00123456U).green == 0x3434,
               "TrueColor pixel query failed");
}

bool
test_surface_raster_and_overlap()
{
    if (!expect(!xmin::server::Surface::create(65535, 65535, 24),
                "oversized surface was accepted")) {
        return false;
    }
    auto surface = xmin::server::Surface::create(4, 2, 24);
    if (!expect(surface.has_value(), "bounded surface creation failed"))
        return false;
    surface->fill(xmin::server::Rectangle{0, 0, 4, 2}, 0x00112233U, 3,
                  0xffffffffU);
    surface->fill(xmin::server::Rectangle{1, 0, 1, 1}, 0x00aabbccU, 3,
                  0x0000ff00U);
    if (!expect(surface->pixel(1, 0) == 0x0011bb33U,
                "surface plane mask was not applied")) {
        return false;
    }

    for (std::int16_t x = 0; x < 4; ++x) {
        surface->fill(xmin::server::Rectangle{x, 1, 1, 1},
                      static_cast<std::uint32_t>(x + 1), 3, 0xffffffffU);
    }
    surface->copy_from(*surface, 0, 1, 1, 1, 3, 1, 3, 0xffffffffU);
    if (!expect(surface->pixel(0, 1) == 1 &&
                    surface->pixel(1, 1) == 1 &&
                    surface->pixel(2, 1) == 2 &&
                    surface->pixel(3, 1) == 3,
                "overlapping surface copy did not use a snapshot")) {
        return false;
    }

    auto lines = xmin::server::Surface::create(8, 8, 24);
    if (!expect(lines.has_value(), "line-test surface creation failed"))
        return false;
    lines->draw_line(std::numeric_limits<std::int32_t>::min(),
                     std::numeric_limits<std::int32_t>::min(),
                     std::numeric_limits<std::int32_t>::max(),
                     std::numeric_limits<std::int32_t>::max(),
                     0x00777777U, 3, 0xffffffffU);
    lines->draw_line(-1000000, 4, 1000000, 4, 0x00abcdefU, 3,
                     0xffffffffU);
    lines->draw_line(-2, -2, 2, 2, 0x00123456U, 3, 0xffffffffU);
    if (!expect(lines->pixel(0, 4) == 0x00abcdefU &&
                    lines->pixel(7, 4) == 0x00abcdefU,
                "large clipped line did not span the surface") ||
        !expect(lines->pixel(0, 0) == 0x00123456U &&
                    lines->pixel(1, 1) == 0x00123456U &&
                    lines->pixel(2, 2) == 0x00123456U &&
                    lines->pixel(7, 7) == 0x00777777U,
                "diagonal clipped line rasterization failed") ||
        !expect(lines->pixel(3, 2) == 0,
                "clipped line escaped its endpoint")) {
        return false;
    }

    auto bitmap = xmin::server::Surface::create(4, 1, 1);
    auto projected = xmin::server::Surface::create(4, 1, 24);
    if (!expect(bitmap && projected,
                "CopyPlane test surface creation failed")) {
        return false;
    }
    bitmap->draw_pixel(0, 0, 1, 3, 1);
    bitmap->draw_pixel(2, 0, 1, 3, 1);
    projected->copy_plane_from(*bitmap, 0, 0, 0, 0, 4, 1, 1,
                               0x00ff0000U, 0x000000ffU, 3,
                               0xffffffffU);
    return expect(projected->pixel(0, 0) == 0x00ff0000U &&
                      projected->pixel(1, 0) == 0x000000ffU &&
                      projected->pixel(2, 0) == 0x00ff0000U &&
                      projected->pixel(3, 0) == 0x000000ffU,
                  "CopyPlane did not map source bits through GC colors");
}

bool
test_region_clipping()
{
    xmin::server::Region region;
    const std::vector<xmin::server::Rectangle> overlapping = {
        {0, 0, 3, 2}, {2, 0, 3, 2}};
    if (!expect(xmin::server::Region::canonicalize(overlapping, region),
                "region canonicalization failed") ||
        !expect(region.contains(0, 0) && region.contains(4, 1) &&
                    !region.contains(5, 1),
                "canonical region bounds are wrong")) {
        return false;
    }

    xmin::server::Region source;
    xmin::server::Region combined;
    const std::vector<xmin::server::Rectangle> second = {{2, 1, 4, 3}};
    if (!expect(xmin::server::Region::canonicalize(second, source),
                "second region canonicalization failed") ||
        !expect(xmin::server::Region::combine(
                    xmin::server::RegionOperation::intersect,
                    region, source, combined),
                "region intersection failed") ||
        !expect(combined.extents().x == 2 && combined.extents().y == 1 &&
                    combined.extents().width == 3 &&
                    combined.extents().height == 1,
                "region intersection extents are wrong") ||
        !expect(xmin::server::Region::combine(
                    xmin::server::RegionOperation::subtract,
                    region, source, combined),
                "region subtraction failed") ||
        !expect(combined.contains(0, 0) && !combined.contains(3, 1),
                "region subtraction contents are wrong") ||
        !expect(xmin::server::Region::combine(
                    xmin::server::RegionOperation::invert,
                    region, source, combined),
                "region inversion failed") ||
        !expect(combined.contains(5, 2) && !combined.contains(3, 1),
                "region inversion contents are wrong") ||
        !expect(combined.translate(-2, 3) && combined.contains(3, 5),
                "region translation failed")) {
        return false;
    }

    auto surface = xmin::server::Surface::create(8, 2, 24);
    if (!expect(surface.has_value(), "clip-test surface creation failed"))
        return false;
    surface->fill({0, 0, 8, 2}, 0x00ff0000U, 3, 0xffffffffU);
    surface->fill({0, 0, 8, 2}, 0x00ffffffU, 6, 0xffffffffU,
                  xmin::server::ClipView{&region, 1, 0});
    if (!expect(surface->pixel(0, 0) == 0x00ff0000U &&
                    surface->pixel(1, 0) == 0x0000ffffU &&
                    surface->pixel(3, 0) == 0x0000ffffU &&
                    surface->pixel(5, 1) == 0x0000ffffU &&
                    surface->pixel(6, 1) == 0x00ff0000U,
                "canonical clip was not applied as a disjoint union")) {
        return false;
    }

    xmin::server::Region empty;
    const std::vector<xmin::server::Rectangle> no_rectangles;
    if (!expect(xmin::server::Region::canonicalize(no_rectangles, empty),
                "empty region canonicalization failed")) {
        return false;
    }
    surface->fill({0, 0, 8, 2}, 0x0000ff00U, 3, 0xffffffffU,
                  xmin::server::ClipView{&empty, 0, 0});
    return expect(surface->pixel(0, 0) == 0x00ff0000U &&
                      surface->pixel(1, 0) == 0x0000ffffU,
                  "empty clip region did not suppress drawing");
}

bool
test_render_engine()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t destination_picture = owner + 1;
    constexpr std::uint32_t blue_picture = owner + 2;
    constexpr std::uint32_t half_red_picture = owner + 3;
    constexpr std::uint32_t alpha_pixmap = owner + 4;
    constexpr std::uint32_t alpha_picture = owner + 5;
    constexpr std::uint32_t clipped_picture = owner + 6;
    constexpr std::uint32_t glyph_set = owner + 7;
    constexpr std::uint32_t glyph_reference = owner + 8;
    constexpr std::uint32_t alpha_user = owner + 9;
    constexpr std::uint32_t linear_picture = owner + 10;
    constexpr std::uint32_t radial_picture = owner + 11;
    constexpr std::uint32_t conical_picture = owner + 12;
    constexpr std::uint32_t binary_pixmap = owner + 13;
    constexpr std::uint32_t binary_picture = owner + 14;
    constexpr std::uint32_t white_picture = owner + 15;
    constexpr std::uint32_t component_mask = owner + 16;
    constexpr std::uint32_t color_pixmap = owner + 17;
    constexpr std::uint32_t alpha_output_pixmap = owner + 18;
    constexpr std::uint32_t alpha_output_picture = owner + 19;
    xmin::server::ServerState server(32, 24);

    if (!expect(xmin::server::render_formats().size() == 4 &&
                    xmin::server::render_format_for_depth(24)->id ==
                        xmin::server::render_xrgb32_format &&
                    xmin::server::render_format(
                        xmin::server::render_a8_format)->direct.alpha_mask ==
                        0xff,
                "RENDER fixed format table is wrong") ||
        !expect(server.add_render_picture(
                    {destination_picture,
                     xmin::server::render_xrgb32_format,
                     xmin::server::RenderDrawableSource{
                         xmin::server::root_window_id}, {}},
                    owner),
                "RENDER destination picture insertion failed") ||
        !expect(server.add_render_picture(
                    {blue_picture, xmin::server::render_argb32_format,
                     xmin::server::RenderSolidSource{
                         {0, 0, 0xffff, 0xffff}}, {}},
                    owner),
                "RENDER blue source insertion failed") ||
        !expect(server.add_render_picture(
                    {half_red_picture, xmin::server::render_argb32_format,
                     xmin::server::RenderSolidSource{
                         {0x8000, 0, 0, 0x8000}}, {}},
                    owner),
                "RENDER alpha source insertion failed")) {
        return false;
    }

    xmin::server::RenderEngine render(server);
    if (!expect(render.composite(
                    1, blue_picture, 0, destination_picture,
                    0, 0, 0, 0, 0, 0, 32, 24) ==
                    xmin::server::RenderStatus::success,
                "RENDER Src composite failed") ||
        !expect(render.composite(
                    3, half_red_picture, 0, destination_picture,
                    0, 0, 0, 0, 0, 0, 32, 24) ==
                    xmin::server::RenderStatus::success,
                "RENDER Over composite failed")) {
        return false;
    }
    const auto *root = server.drawable_surface(xmin::server::root_window_id);
    const std::uint32_t blended = root->pixel(16, 12);
    if (!expect(((blended >> 16) & 0xffU) >= 127 &&
                    ((blended >> 16) & 0xffU) <= 129 &&
                    (blended & 0xffU) >= 127 &&
                    (blended & 0xffU) <= 129,
                "RENDER premultiplied alpha blend is wrong") ||
        !expect(render.composite(
                    14, blue_picture, 0, destination_picture,
                    0, 0, 0, 0, 0, 0, 1, 1) ==
                    xmin::server::RenderStatus::bad_operator,
                "RENDER accepted a reserved operator")) {
        return false;
    }

    std::vector<xmin::server::Rectangle> clip_rectangles{{2, 3, 4, 5}};
    xmin::server::Region clip;
    if (!expect(xmin::server::Region::canonicalize(clip_rectangles, clip),
                "RENDER clip construction failed")) {
        return false;
    }
    xmin::server::RenderPictureAttributes clipped_attributes;
    clipped_attributes.clip = std::move(clip);
    if (!expect(server.add_render_picture(
                    {clipped_picture, xmin::server::render_xrgb32_format,
                     xmin::server::RenderDrawableSource{
                         xmin::server::root_window_id},
                     std::move(clipped_attributes)},
                    owner),
                "RENDER clipped destination insertion failed") ||
        !expect(render.fill_rectangles(
                    1, clipped_picture, {0, 0xffff, 0, 0xffff},
                    {{0, 0, 32, 24}}) == xmin::server::RenderStatus::success,
                "RENDER clipped FillRectangles failed") ||
        !expect((root->pixel(3, 4) & 0x00ffffffU) == 0x0000ff00U &&
                    (root->pixel(1, 4) & 0x00ffffffU) != 0x0000ff00U,
                "RENDER picture clip escaped its region")) {
        return false;
    }

    auto alpha_surface = xmin::server::Surface::create(7, 3, 8);
    if (!expect(alpha_surface.has_value(),
                "RENDER A8 surface creation failed")) {
        return false;
    }

    xmin::server::CursorImage cursor_image;
    cursor_image.pixels = {0, 0, 0};
    cursor_image.pixel_roles = {0, 1, 2};
    cursor_image.recolor(
        {0xffff, 0, 0, 0xffff}, {0, 0xffff, 0, 0xffff});
    if (!expect(cursor_image.pixels[0] == 0 &&
                    cursor_image.pixels[1] == 0xff00ff00U &&
                    cursor_image.pixels[2] == 0xffff0000U,
                "core cursor recoloring did not rewrite role pixels")) {
        return false;
    }
    auto managed_alpha = server.adopt_surface(std::move(*alpha_surface));
    if (!expect(managed_alpha != nullptr,
                "RENDER A8 surface adoption failed") ||
        !expect(server.add_pixmap(
                    {alpha_pixmap, std::move(managed_alpha)}, owner),
                "RENDER A8 pixmap insertion failed") ||
        !expect(server.add_render_picture(
                    {alpha_picture, xmin::server::render_a8_format,
                     xmin::server::RenderDrawableSource{alpha_pixmap}, {}},
                    owner),
                "RENDER A8 picture insertion failed") ||
        !expect(render.fill_rectangles(
                    1, alpha_picture, {0, 0, 0, 0x8080},
                    {{1, 1, 5, 1}}) == xmin::server::RenderStatus::success,
                "RENDER packed A8 destination failed") ||
        !expect(server.drawable_surface(alpha_pixmap)->pixel(0, 1) == 0 &&
                    server.drawable_surface(alpha_pixmap)->pixel(3, 1) >=
                        0x7f &&
                    server.drawable_surface(alpha_pixmap)->pixel(3, 1) <=
                        0x81 &&
                    server.drawable_surface(alpha_pixmap)->pixel(6, 1) == 0,
                "RENDER packed A8 write-back is wrong")) {
        return false;
    }

    xmin::server::RenderPictureAttributes alpha_attributes;
    alpha_attributes.alpha_map = alpha_picture;
    const auto *alpha_resource = server.render_picture(alpha_picture);
    const auto *alpha_drawable = alpha_resource == nullptr
        ? nullptr
        : std::get_if<xmin::server::RenderDrawableSource>(
              &alpha_resource->source);
    auto color_surface = xmin::server::Surface::create(7, 3, 32);
    if (!expect(color_surface.has_value(),
                "RENDER alpha-mapped color surface creation failed")) {
        return false;
    }
    color_surface->fill(
        {0, 0, 7, 3}, 0xff0000ffU, 3, 0xffffffffU);
    auto managed_color = server.adopt_surface(std::move(*color_surface));
    auto output_surface = xmin::server::Surface::create(7, 3, 32);
    if (!expect(output_surface.has_value(),
                "RENDER alpha output surface creation failed")) {
        return false;
    }
    auto managed_output = server.adopt_surface(std::move(*output_surface));
    if (!expect(alpha_drawable != nullptr && alpha_drawable->pixmap,
                "RENDER alpha picture did not retain pixmap identity") ||
        !expect(managed_color != nullptr &&
                    server.add_pixmap(
                        {color_pixmap, std::move(managed_color)}, owner),
                "RENDER alpha-mapped color pixmap insertion failed") ||
        !expect(managed_output != nullptr &&
                    server.add_pixmap(
                        {alpha_output_pixmap, std::move(managed_output)},
                        owner),
                "RENDER alpha output pixmap insertion failed") ||
        !expect(server.add_render_picture(
                    {alpha_output_picture,
                     xmin::server::render_argb32_format,
                     xmin::server::RenderDrawableSource{alpha_output_pixmap},
                     {}},
                    owner),
                "RENDER alpha output picture insertion failed") ||
        !expect(server.add_render_picture(
                    {alpha_user, xmin::server::render_argb32_format,
                     xmin::server::RenderDrawableSource{color_pixmap},
                     std::move(alpha_attributes)},
                    owner),
                "RENDER alpha-map user insertion failed")) {
        return false;
    }
    const auto retained_alpha =
        server.render_picture(alpha_user)->attributes.alpha_map_picture;
    if (!expect(retained_alpha != nullptr,
                "RENDER alpha-map reference was not retained")) {
        return false;
    }
    const auto *retained_source = std::get_if<
        xmin::server::RenderDrawableSource>(&retained_alpha->source);
    if (!expect(retained_source != nullptr,
                "RENDER retained alpha map lost its drawable")) {
        return false;
    }
    const auto retained_surface = retained_source->surface;
    const xmin::server::RenderTrap alpha_trap{
        {1 * 65536, 5 * 65536, 0 * 65536},
        {1 * 65536, 5 * 65536, 1 * 65536}};
    if (!expect(render.add_traps(alpha_picture, 0, 0, {alpha_trap}) ==
                    xmin::server::RenderStatus::success,
                "RENDER AddTraps rasterization failed") ||
        !expect(retained_surface->pixel(2, 0) != 0,
                "RENDER AddTraps produced no coverage") ||
        !expect(server.erase_render_picture(alpha_picture),
                "RENDER alpha-map public resource removal failed") ||
        !expect(server.erase_pixmap(alpha_pixmap),
                "RENDER drawable public resource removal failed") ||
        !expect(server.render_picture(alpha_picture) == nullptr &&
                    server.pixmap(alpha_pixmap) == nullptr,
                "RENDER freed IDs remained public") ||
        !expect(retained_surface != nullptr,
                "RENDER references did not retain their resources") ||
        !expect(render.composite(
                    1, alpha_user, 0, alpha_output_picture,
                    0, 0, 0, 0, 0, 0, 7, 3) ==
                    xmin::server::RenderStatus::success,
                "RENDER retained alpha map was unusable") ||
        !expect((server.drawable_surface(alpha_output_pixmap)->pixel(0, 1) >>
                     24) == 0 &&
                    (server.drawable_surface(alpha_output_pixmap)->pixel(
                         3, 1) >> 24) >= 0x7f &&
                    (server.drawable_surface(alpha_output_pixmap)->pixel(
                         3, 1) >> 24) <= 0x81,
                "RENDER alpha-map sampling produced wrong coverage")) {
        return false;
    }

    const xmin::server::RenderTrapezoid trapezoid{
        2 * 65536, 14 * 65536,
        {{4 * 65536, 2 * 65536}, {4 * 65536, 14 * 65536}},
        {{16 * 65536, 2 * 65536}, {16 * 65536, 14 * 65536}}};
    if (!expect(render.fill_rectangles(
                    1, destination_picture, {0, 0, 0, 0xffff},
                    {{0, 0, 32, 24}}) == xmin::server::RenderStatus::success,
                "RENDER background clear failed") ||
        !expect(render.composite_trapezoids(
                    3, blue_picture, destination_picture,
                    xmin::server::render_a8_format, 0, 0, {trapezoid}) ==
                    xmin::server::RenderStatus::success,
                "RENDER trapezoid composite failed") ||
        !expect((root->pixel(8, 8) & 0x00ffffffU) == 0x000000ffU &&
                    (root->pixel(20, 8) & 0x00ffffffU) == 0,
                "RENDER trapezoid coverage is wrong")) {
        return false;
    }

    const std::vector<xmin::server::RenderGradientStop> gradient_stops{
        {0, {0xffff, 0, 0, 0xffff}},
        {65536, {0, 0, 0xffff, 0xffff}}};
    if (!expect(server.add_render_picture(
                    {linear_picture, xmin::server::render_argb32_format,
                     xmin::server::RenderLinearGradient{
                         {0, 0}, {32 * 65536, 0}, gradient_stops}, {}},
                    owner),
                "RENDER linear gradient insertion failed") ||
        !expect(render.composite(
                    1, linear_picture, 0, destination_picture,
                    0, 0, 0, 0, 0, 0, 32, 24) ==
                    xmin::server::RenderStatus::success,
                "RENDER linear gradient composite failed")) {
        return false;
    }
    const std::uint32_t linear_left = root->pixel(1, 12);
    const std::uint32_t linear_right = root->pixel(30, 12);
    auto *linear_resource = server.render_picture(linear_picture);
    linear_resource->attributes.transform[2] = 8 * 65536;
    linear_resource->attributes.filter = xmin::server::RenderFilter::bilinear;
    if (!expect(((linear_left >> 16) & 0xffU) > (linear_left & 0xffU) &&
                    (linear_right & 0xffU) >
                        ((linear_right >> 16) & 0xffU),
                "RENDER linear gradient samples are wrong") ||
        !expect(render.composite(
                    1, linear_picture, 0, destination_picture,
                    0, 0, 0, 0, 0, 0, 32, 24) ==
                    xmin::server::RenderStatus::success,
                "RENDER transformed bilinear gradient failed") ||
        !expect((root->pixel(1, 12) & 0xffU) > (linear_left & 0xffU),
                "RENDER picture transform did not shift sampling")) {
        return false;
    }
    linear_resource->attributes.transform[2] = 0;
    linear_resource->attributes.repeat = xmin::server::RenderRepeat::normal;
    std::get<xmin::server::RenderLinearGradient>(
        linear_resource->source).p2.x = 8 * 65536;
    if (!expect(render.composite(
                    1, linear_picture, 0, destination_picture,
                    0, 0, 0, 0, 0, 0, 32, 24) ==
                    xmin::server::RenderStatus::success,
                "RENDER repeated gradient failed") ||
        !expect(root->pixel(1, 12) == root->pixel(9, 12),
                "RENDER normal repeat did not tile")) {
        return false;
    }

    if (!expect(server.add_render_picture(
                    {radial_picture, xmin::server::render_argb32_format,
                     xmin::server::RenderRadialGradient{
                         {16 * 65536, 12 * 65536},
                         {16 * 65536, 12 * 65536}, 0, 16 * 65536,
                         gradient_stops}, {}},
                    owner),
                "RENDER radial gradient insertion failed") ||
        !expect(render.composite(
                    1, radial_picture, 0, destination_picture,
                    0, 0, 0, 0, 0, 0, 32, 24) ==
                    xmin::server::RenderStatus::success,
                "RENDER radial gradient composite failed") ||
        !expect(root->pixel(16, 12) != root->pixel(30, 12),
                "RENDER radial gradient produced a flat image") ||
        !expect(server.add_render_picture(
                    {conical_picture, xmin::server::render_argb32_format,
                     xmin::server::RenderConicalGradient{
                         {16 * 65536, 12 * 65536}, 0, gradient_stops}, {}},
                    owner),
                "RENDER conical gradient insertion failed") ||
        !expect(render.composite(
                    1, conical_picture, 0, destination_picture,
                    0, 0, 0, 0, 0, 0, 32, 24) ==
                    xmin::server::RenderStatus::success,
                "RENDER conical gradient composite failed") ||
        !expect(root->pixel(24, 12) != root->pixel(8, 12),
                "RENDER conical gradient produced a flat image")) {
        return false;
    }

    xmin::server::RenderPictureAttributes component_attributes;
    component_attributes.component_alpha = true;
    if (!expect(server.add_render_picture(
                    {white_picture, xmin::server::render_argb32_format,
                     xmin::server::RenderSolidSource{
                         {0xffff, 0xffff, 0xffff, 0xffff}}, {}},
                    owner),
                "RENDER component-alpha source insertion failed") ||
        !expect(server.add_render_picture(
                    {component_mask, xmin::server::render_argb32_format,
                     xmin::server::RenderSolidSource{
                         {0xffff, 0, 0, 0xffff}},
                     std::move(component_attributes)},
                    owner),
                "RENDER component-alpha mask insertion failed") ||
        !expect(render.fill_rectangles(
                    1, destination_picture, {0, 0, 0, 0xffff},
                    {{0, 0, 32, 24}}) == xmin::server::RenderStatus::success,
                "RENDER component-alpha background clear failed") ||
        !expect(render.composite(
                    3, white_picture, component_mask, destination_picture,
                    0, 0, 0, 0, 0, 0, 1, 1) ==
                    xmin::server::RenderStatus::success,
                "RENDER component-alpha composite failed") ||
        !expect((root->pixel(0, 0) & 0x00ffffffU) == 0x00ff0000U,
                "RENDER component-alpha channels were not independent")) {
        return false;
    }

    auto binary_surface = xmin::server::Surface::create(5, 1, 1);
    if (!expect(binary_surface.has_value(),
                "RENDER A1 surface creation failed")) {
        return false;
    }
    auto managed_binary = server.adopt_surface(std::move(*binary_surface));
    if (!expect(managed_binary != nullptr &&
                    server.add_pixmap(
                        {binary_pixmap, std::move(managed_binary)}, owner),
                "RENDER A1 pixmap insertion failed") ||
        !expect(server.add_render_picture(
                    {binary_picture, xmin::server::render_a1_format,
                     xmin::server::RenderDrawableSource{binary_pixmap}, {}},
                    owner),
                "RENDER A1 picture insertion failed") ||
        !expect(render.fill_rectangles(
                    1, binary_picture, {0, 0, 0, 0xffff},
                    {{1, 0, 3, 1}}) == xmin::server::RenderStatus::success,
                "RENDER native A1 write failed") ||
        !expect(server.drawable_surface(binary_pixmap)->pixel(0, 0) == 0 &&
                    server.drawable_surface(binary_pixmap)->pixel(1, 0) == 1 &&
                    server.drawable_surface(binary_pixmap)->pixel(3, 0) == 1 &&
                    server.drawable_surface(binary_pixmap)->pixel(4, 0) == 0,
                "RENDER native A1 layout or write-back is wrong")) {
        return false;
    }

    auto storage = std::make_shared<xmin::server::RenderGlyphStorage>();
    storage->format = xmin::server::render_a8_format;
    if (!expect(server.add_render_glyph_set(
                    {glyph_set, storage}, owner),
                "RENDER glyph set insertion failed") ||
        !expect(server.add_render_glyph_set(
                    {glyph_reference, storage}, owner),
                "RENDER glyph set reference insertion failed")) {
        return false;
    }
    storage->glyphs.emplace(
        1, xmin::server::RenderGlyph{{1, 1, 0, 0, 1, 0}, {0xff}});
    if (!expect(server.render_glyph_set(glyph_reference)->storage->
                    glyphs.count(1) == 1,
                "RENDER glyph set reference did not share storage")) {
        return false;
    }

    server.disconnect_client(owner);
    return expect(server.render_picture(destination_picture) == nullptr &&
                      server.render_glyph_set(glyph_reference) == nullptr &&
                      server.pixmap(alpha_pixmap) == nullptr,
                  "RENDER resources survived owner disconnect");
}

bool
test_scene_composition()
{
    constexpr std::uint32_t owner = 0x00200000;
    xmin::server::ServerState server(16, 12);
    auto *root = server.window(xmin::server::root_window_id);
    root->surface->fill({0, 0, 16, 12}, 0x000000ffU, 3, 0xffffffffU);
    server.invalidate_scene();

    auto parent_surface = xmin::server::Surface::create(8, 6, 24);
    auto child_surface = xmin::server::Surface::create(4, 4, 24);
    if (!expect(parent_surface && child_surface,
                "scene test surface allocation failed")) {
        return false;
    }
    parent_surface->fill({0, 0, 8, 6}, 0x0000ff00U, 3, 0xffffffffU);
    child_surface->fill({0, 0, 4, 4}, 0x00ffff00U, 3, 0xffffffffU);

    xmin::server::WindowRecord parent;
    parent.id = owner;
    parent.parent = xmin::server::root_window_id;
    parent.x = 2;
    parent.y = 1;
    parent.width = 8;
    parent.height = 6;
    parent.border_width = 1;
    parent.border_pixel = 0x00ff0000U;
    parent.mapped = true;
    parent.surface = server.adopt_surface(std::move(*parent_surface));
    xmin::server::WindowRecord child;
    child.id = owner + 1;
    child.parent = owner;
    child.x = 6;
    child.y = 4;
    child.width = 4;
    child.height = 4;
    child.mapped = true;
    child.surface = server.adopt_surface(std::move(*child_surface));
    if (!expect(server.add_window(std::move(parent), owner),
                "scene parent insertion failed") ||
        !expect(server.add_window(std::move(child), owner),
                "scene child insertion failed")) {
        return false;
    }

    const auto *composed = server.readable_surface(xmin::server::root_window_id);
    if (!expect(composed != nullptr, "composed root is missing") ||
        !expect(composed->pixel(0, 0) == 0x000000ffU,
                "root backing pixel was not preserved") ||
        !expect(composed->pixel(2, 1) == 0x00ff0000U,
                "window border was not composed") ||
        !expect(composed->pixel(3, 2) == 0x0000ff00U,
                "window content was not composed") ||
        !expect(composed->pixel(9, 6) == 0x00ffff00U,
                "nested window content was not composed") ||
        !expect(composed->pixel(11, 6) == 0x00ff0000U,
                "child content escaped its parent clip")) {
        return false;
    }

    root->surface->fill({0, 0, 1, 1}, 0x00ffffffU, 3, 0xffffffffU);
    root->surface->fill({15, 11, 1, 1}, 0x00ffffffU, 3, 0xffffffffU);
    server.invalidate_scene();
    composed = server.readable_surface(
        xmin::server::root_window_id, {0, 0, 1, 1});
    if (!expect(composed->pixel(0, 0) == 0x00ffffffU,
                "partial root composition omitted its requested area")) {
        return false;
    }
    composed = server.readable_surface(xmin::server::root_window_id);
    if (!expect(composed->pixel(15, 11) == 0x00ffffffU,
                "partial root composition incorrectly marked the scene clean")) {
        return false;
    }
    root->surface->fill({0, 0, 16, 12}, 0x000000ffU, 3, 0xffffffffU);
    server.invalidate_scene();

    static_cast<void>(
        server.set_window_mapped(*server.window(owner), false));
    composed = server.readable_surface(xmin::server::root_window_id);
    if (!expect(composed->pixel(3, 2) == 0x000000ffU,
                "unmapped window remained in the scene")) {
        return false;
    }
    static_cast<void>(
        server.set_window_mapped(*server.window(owner), true));
    composed = server.readable_surface(xmin::server::root_window_id);
    if (!expect(composed->pixel(3, 2) == 0x0000ff00U,
                "remapped window did not return to the scene")) {
        return false;
    }

    xmin::server::Region bounding;
    xmin::server::Region clip;
    const std::vector<xmin::server::Rectangle> bounding_rectangles = {
        {0, 0, 3, 3}};
    const std::vector<xmin::server::Rectangle> clip_rectangles = {
        {1, 1, 1, 1}};
    auto *stored_parent = server.window(owner);
    if (!expect(xmin::server::Region::canonicalize(
                    bounding_rectangles, bounding) &&
                    xmin::server::Region::canonicalize(clip_rectangles, clip),
                "scene shape canonicalization failed") ||
        !expect(server.set_window_shape(
                    *stored_parent, 0, std::move(bounding)) ==
                    xmin::server::ShapeUpdate::updated &&
                    server.set_window_shape(*stored_parent, 1,
                                            std::move(clip)) ==
                    xmin::server::ShapeUpdate::updated,
                "scene shape update failed")) {
        return false;
    }
    composed = server.readable_surface(xmin::server::root_window_id);
    return expect(composed->pixel(4, 3) == 0x0000ff00U,
                  "shape intersection omitted visible window content") &&
        expect(composed->pixel(3, 2) == 0x000000ffU,
               "clip hole retained window pixels") &&
        expect(composed->pixel(2, 1) == 0x000000ffU,
               "bounding shape retained an excluded border") &&
        expect(composed->pixel(9, 6) == 0x000000ffU,
               "bounding shape did not constrain child composition");
}

bool
test_shape_state()
{
    constexpr std::uint32_t client = 0x00200000;
    constexpr std::uint32_t window_id = 0x00400000;
    xmin::server::ServerState server(16, 12);
    if (!expect(server.register_client(client),
                "shape client registration failed")) {
        return false;
    }
    server.note_client_sequence(client, 41);
    xmin::server::WindowRecord window;
    window.id = window_id;
    window.parent = xmin::server::root_window_id;
    window.width = 8;
    window.height = 6;
    if (!expect(server.add_window(std::move(window), 0),
                "shape window insertion failed")) {
        return false;
    }
    auto *stored = server.window(window_id);
    xmin::server::Region initial;
    const std::vector<xmin::server::Rectangle> initial_rectangles = {
        {1, 2, 3, 4}};
    if (!expect(server.select_shape_events(*stored, client, true),
                "shape event selection failed") ||
        !expect(xmin::server::Region::canonicalize(
                    initial_rectangles, initial),
                "initial shape canonicalization failed") ||
        !expect(server.set_window_shape(*stored, 0, std::move(initial)) ==
                    xmin::server::ShapeUpdate::updated,
                "initial shape update failed")) {
        return false;
    }
    const auto *event = server.next_event(client);
    const auto *notify = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::ShapeNotifyEvent>(event);
    if (!expect(notify != nullptr && notify->sequence == 41 &&
                    notify->kind == 0 && notify->window == window_id &&
                    notify->x == 1 && notify->y == 2 &&
                    notify->width == 3 && notify->height == 4 &&
                    notify->shaped,
                "shape notification lost typed state")) {
        return false;
    }
    server.pop_event(client);
    for (std::size_t count = 0;
         count < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.broadcast_mapping_notify(1, 96, 1),
                    "shape queue setup failed")) {
            return false;
        }
    }

    xmin::server::Region replacement;
    const std::vector<xmin::server::Rectangle> replacement_rectangles = {
        {0, 0, 1, 1}};
    if (!expect(xmin::server::Region::canonicalize(
                    replacement_rectangles, replacement),
                "replacement shape canonicalization failed") ||
        !expect(server.set_window_shape(
                    *stored, 0, std::move(replacement)) ==
                    xmin::server::ShapeUpdate::queue_full,
                "full observer queue accepted a shape update") ||
        !expect(stored->shapes[0] &&
                    stored->shapes[0]->extents().x == 1 &&
                    stored->shapes[0]->extents().width == 3,
                "failed shape notification partially committed state")) {
        return false;
    }

    server.disconnect_client(client);
    xmin::server::Region disconnected_update;
    if (!expect(xmin::server::Region::canonicalize(
                    replacement_rectangles, disconnected_update),
                "disconnected shape canonicalization failed") ||
        !expect(server.set_window_shape(
                    *stored, 0, std::move(disconnected_update)) ==
                    xmin::server::ShapeUpdate::updated,
                "disconnect retained a stale shape subscription") ||
        !expect(!server.has_pending_event(client),
                "disconnect retained a shape notification queue")) {
        return false;
    }
    return true;
}

bool
test_window_tree_mutations()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t picture_id = owner + 2;
    xmin::server::ServerState server(32, 24);
    xmin::server::WindowRecord parent;
    parent.id = owner;
    parent.parent = xmin::server::root_window_id;
    xmin::server::WindowRecord child;
    child.id = owner + 1;
    child.parent = owner;
    child.mapped = true;
    child.width = 2;
    child.height = 2;
    auto child_surface = xmin::server::Surface::create(2, 2, 24);
    if (!expect(child_surface.has_value(),
                "tree child surface allocation failed")) {
        return false;
    }
    child.surface = server.adopt_surface(std::move(*child_surface));
    const auto retained_surface = child.surface;
    if (!expect(server.add_window(std::move(parent), owner),
                "tree parent insertion failed") ||
        !expect(server.add_window(std::move(child), owner),
                "tree child insertion failed") ||
        !expect(server.add_render_picture(
                    {picture_id, xmin::server::render_xrgb32_format,
                     xmin::server::RenderDrawableSource{owner + 1}, {}},
                    owner),
                "tree child RENDER picture insertion failed")) {
        return false;
    }
    static_cast<void>(server.set_subwindows_mapped(owner, false));
    if (!expect(!server.window(owner + 1)->mapped,
                "UnmapSubwindows state transition failed")) {
        return false;
    }
    static_cast<void>(server.set_subwindows_mapped(owner, true));
    if (!expect(server.reparent_window(owner, owner + 1, 0, 0) ==
                    xmin::server::ReparentUpdate::invalid,
                "window cycle was accepted") ||
        !expect(server.reparent_window(
                    owner + 1, xmin::server::root_window_id, 7, 8) ==
                    xmin::server::ReparentUpdate::updated,
                "window reparenting failed") ||
        !expect(server.window(owner + 1)->parent ==
                    xmin::server::root_window_id,
                "reparented window retained its old parent") ||
        !expect(server.window(owner)->children.empty(),
                "old parent retained a reparented child") ||
        !expect(server.reparent_window(owner + 1, owner, 1, 2) ==
                    xmin::server::ReparentUpdate::updated,
                "window reparenting back to its owner failed")) {
        return false;
    }
    static_cast<void>(server.destroy_subwindows(owner));
    const auto *picture = server.render_picture(picture_id);
    const auto *drawable = picture == nullptr
        ? nullptr
        : std::get_if<xmin::server::RenderDrawableSource>(&picture->source);
    return expect(server.window(owner + 1) == nullptr,
                  "DestroySubwindows retained a child") &&
        expect(server.window(owner) != nullptr,
               "DestroySubwindows removed its parent") &&
        expect(drawable != nullptr && drawable->surface == retained_surface,
               "window destruction invalidated its live RENDER picture") &&
        expect(server.erase_render_picture(picture_id),
               "live RENDER picture could not be freed after window destruction");
}

bool
test_sync_state()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t waiter = 0x00400000;
    constexpr std::uint32_t counter_id = owner + 1;
    constexpr std::uint32_t alarm_id = owner + 2;
    constexpr std::uint32_t fence_id = owner + 3;
    xmin::server::ServerState server(32, 24);
    if (!expect(server.register_client(owner) && server.register_client(waiter),
                "SYNC clients could not be registered") ||
        !expect(server.add_sync_counter({counter_id, 0}, owner),
                "SYNC counter insertion failed")) {
        return false;
    }

    xmin::server::SyncTrigger wait_trigger;
    wait_trigger.counter = counter_id;
    wait_trigger.wait_value = 2;
    wait_trigger.test_value = 2;
    wait_trigger.test_type = xmin::server::SyncTestType::positive_comparison;
    std::vector<xmin::server::SyncWaitCondition> conditions{
        {wait_trigger, 0}};
    if (!expect(server.begin_sync_counter_await(
                    waiter, std::move(conditions)) ==
                    xmin::server::SyncUpdate::updated &&
                   server.sync_waiting(waiter),
                "SYNC Await did not suspend its client")) {
        return false;
    }
    auto *counter = server.sync_counter(counter_id);
    if (!expect(counter != nullptr &&
                   server.set_sync_counter(*counter, 1) ==
                       xmin::server::SyncUpdate::updated &&
                   server.sync_waiting(waiter),
                "SYNC Await resumed before its trigger") ||
        !expect(server.set_sync_counter(*counter, 2) ==
                    xmin::server::SyncUpdate::updated &&
                   !server.sync_waiting(waiter),
                "SYNC Await did not resume at its trigger")) {
        return false;
    }
    const auto *event = server.next_event(waiter);
    const auto *counter_event = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::SyncCounterNotifyEvent>(event);
    if (!expect(counter_event != nullptr &&
                   counter_event->counter == counter_id &&
                   counter_event->wait_value == 2 &&
                   counter_event->counter_value == 2 &&
                   counter_event->count == 0 && !counter_event->destroyed,
                "SYNC Await emitted the wrong CounterNotify")) {
        return false;
    }
    server.pop_event(waiter);

    xmin::server::SyncAlarmRecord alarm;
    alarm.id = alarm_id;
    alarm.trigger.counter = counter_id;
    alarm.trigger.wait_value = 3;
    alarm.trigger.test_value = 3;
    alarm.trigger.test_type =
        xmin::server::SyncTestType::positive_comparison;
    alarm.delta = 2;
    alarm.event_clients.push_back(owner);
    if (!expect(server.add_sync_alarm(std::move(alarm), owner) ==
                    xmin::server::SyncUpdate::updated,
                "SYNC alarm insertion failed") ||
        !expect(server.set_sync_counter(*counter, 7) ==
                    xmin::server::SyncUpdate::updated,
                "SYNC alarm counter update failed")) {
        return false;
    }
    event = server.next_event(owner);
    const auto *alarm_event = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::SyncAlarmNotifyEvent>(event);
    const auto *stored_alarm = server.sync_alarm(alarm_id);
    if (!expect(alarm_event != nullptr && alarm_event->alarm == alarm_id &&
                   alarm_event->counter_value == 7 &&
                   alarm_event->alarm_value == 3 && alarm_event->state == 0,
                "SYNC alarm emitted the wrong AlarmNotify") ||
        !expect(stored_alarm != nullptr &&
                   stored_alarm->trigger.test_value == 9 &&
                   stored_alarm->state == 0,
                "SYNC alarm did not advance beyond the counter")) {
        return false;
    }
    server.pop_event(owner);

    for (std::size_t index = 0;
         index < xmin::server::maximum_pending_events_per_client; ++index) {
        if (!expect(server.broadcast_mapping_notify(0, 0, 0),
                    "SYNC queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.set_sync_counter(*counter, 9) ==
                    xmin::server::SyncUpdate::queue_full,
                "SYNC alarm update ignored event queue pressure") ||
        !expect(counter->value == 7 &&
                   server.sync_alarm(alarm_id)->trigger.test_value == 9,
                "SYNC queue pressure left a partial counter/alarm update")) {
        return false;
    }
    for (std::size_t index = 0;
         index < xmin::server::maximum_pending_events_per_client; ++index) {
        server.pop_event(owner);
        server.pop_event(waiter);
    }

    conditions.assign(1, xmin::server::SyncWaitCondition{wait_trigger, 0});
    wait_trigger.test_value = 100;
    conditions[0].trigger = wait_trigger;
    if (!expect(server.begin_sync_counter_await(
                    waiter, std::move(conditions)) ==
                    xmin::server::SyncUpdate::updated,
                "SYNC destruction Await setup failed") ||
        !expect(server.erase_sync_counter(counter_id) ==
                    xmin::server::SyncUpdate::updated,
                "SYNC counter destruction failed")) {
        return false;
    }
    event = server.next_event(waiter);
    counter_event = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::SyncCounterNotifyEvent>(event);
    stored_alarm = server.sync_alarm(alarm_id);
    if (!expect(counter_event != nullptr && counter_event->destroyed &&
                   counter_event->counter_value == 7 &&
                   !server.sync_waiting(waiter),
                "counter destruction did not release SYNC Await") ||
        !expect(stored_alarm != nullptr &&
                   stored_alarm->trigger.counter == 0 &&
                   stored_alarm->state == 1,
                "counter destruction did not inactivate its alarm")) {
        return false;
    }
    server.pop_event(waiter);
    event = server.next_event(owner);
    alarm_event = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::SyncAlarmNotifyEvent>(event);
    if (!expect(alarm_event != nullptr && alarm_event->state == 1,
                "counter destruction omitted AlarmNotify")) {
        return false;
    }
    server.pop_event(owner);

    if (!expect(server.add_sync_fence({fence_id, false}, owner),
                "SYNC fence insertion failed") ||
        !expect(server.begin_sync_fence_await(waiter, {fence_id}) ==
                    xmin::server::SyncUpdate::updated &&
                   server.sync_waiting(waiter),
                "SYNC AwaitFence did not suspend its client") ||
        !expect(server.trigger_sync_fence(fence_id) ==
                    xmin::server::SyncUpdate::updated &&
                   !server.sync_waiting(waiter),
                "SYNC fence trigger did not resume AwaitFence") ||
        !expect(server.reset_sync_fence(fence_id),
                "SYNC fence reset failed") ||
        !expect(!server.reset_sync_fence(fence_id),
                "SYNC accepted reset of an untriggered fence")) {
        return false;
    }

    server.set_sync_priority(waiter, -7);
    if (!expect(server.sync_priority(waiter) == -7,
                "SYNC priority was not retained")) {
        return false;
    }

    constexpr std::uint32_t boundary_counter = owner + 4;
    constexpr std::uint32_t boundary_alarm = owner + 5;
    if (!expect(server.add_sync_counter(
                    {boundary_counter,
                     std::numeric_limits<std::int64_t>::max()}, owner),
                "SYNC boundary counter insertion failed")) {
        return false;
    }
    xmin::server::SyncAlarmRecord overflow_alarm;
    overflow_alarm.id = boundary_alarm;
    overflow_alarm.trigger.counter = boundary_counter;
    overflow_alarm.trigger.wait_value =
        std::numeric_limits<std::int64_t>::max();
    overflow_alarm.trigger.test_value =
        std::numeric_limits<std::int64_t>::max();
    overflow_alarm.trigger.test_type =
        xmin::server::SyncTestType::positive_comparison;
    overflow_alarm.delta = 1;
    overflow_alarm.event_clients.push_back(owner);
    if (!expect(server.add_sync_alarm(std::move(overflow_alarm), owner) ==
                    xmin::server::SyncUpdate::updated,
                "SYNC boundary alarm insertion failed")) {
        return false;
    }
    const auto *bounded_alarm = server.sync_alarm(boundary_alarm);
    if (!expect(bounded_alarm != nullptr && bounded_alarm->state == 1 &&
                   bounded_alarm->trigger.test_value ==
                       std::numeric_limits<std::int64_t>::max(),
                "SYNC alarm overflow did not become inactive")) {
        return false;
    }
    server.pop_event(owner);
    if (!expect(server.erase_sync_alarm(boundary_alarm) ==
                    xmin::server::SyncUpdate::updated,
                "SYNC boundary alarm cleanup failed")) {
        return false;
    }
    server.pop_event(owner);
    if (!expect(server.erase_sync_counter(boundary_counter) ==
                    xmin::server::SyncUpdate::updated,
                "SYNC boundary counter cleanup failed")) {
        return false;
    }
    server.disconnect_client(owner);
    return expect(server.sync_alarm(alarm_id) == nullptr &&
                      server.sync_fence(fence_id) == nullptr,
                  "SYNC resources survived owner disconnect");
}

bool
test_xfixes_state()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t observer = 0x00400000;
    constexpr std::uint32_t parent_id = owner + 1;
    constexpr std::uint32_t child_id = observer + 1;
    constexpr std::uint32_t source_cursor_id = owner + 2;
    constexpr std::uint32_t target_cursor_id = owner + 3;
    constexpr std::uint32_t region_id = owner + 4;
    constexpr std::uint32_t barrier_id = owner + 5;
    xmin::server::ServerState server(64, 48);
    if (!expect(server.register_client(owner) &&
                    server.register_client(observer),
                "XFIXES client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 17);
    server.note_client_sequence(observer, 31);

    xmin::server::WindowRecord parent;
    parent.id = parent_id;
    parent.parent = xmin::server::root_window_id;
    parent.width = 20;
    parent.height = 16;
    xmin::server::WindowRecord child;
    child.id = child_id;
    child.parent = parent_id;
    child.x = 3;
    child.y = 4;
    child.width = 8;
    child.height = 6;
    if (!expect(server.add_window(std::move(parent), owner) &&
                    server.add_window(std::move(child), observer),
                "XFIXES save-set window insertion failed")) {
        return false;
    }
    static_cast<void>(
        server.set_window_mapped(*server.window(parent_id), true));
    if (!expect(server.alter_save_set(
                    owner, child_id, true, true, true) ==
                    xmin::server::XFixesUpdate::updated,
                "XFIXES save-set insertion failed")) {
        return false;
    }

    xmin::server::Region region;
    const std::vector<xmin::server::Rectangle> rectangles{{2, 3, 9, 7}};
    if (!expect(xmin::server::Region::canonicalize(rectangles, region) &&
                    server.add_xfixes_region(
                        region_id, std::move(region), owner),
                "typed XFIXES region insertion failed") ||
        !expect(server.xfixes_region(region_id) != nullptr &&
                    server.xfixes_region(region_id)->extents().width == 9,
                "typed XFIXES region lookup failed")) {
        return false;
    }

    const auto selection = server.atoms().intern("XMIN_XFIXES_SELECTION");
    if (!expect(server.select_xfixes_selection_input(
                    observer, xmin::server::root_window_id, selection, 7) ==
                    xmin::server::XFixesUpdate::updated,
                "XFIXES selection subscription failed") ||
        !expect(server.set_selection_owner(
                    selection, parent_id, owner, 0) ==
                    xmin::server::SelectionUpdate::updated,
                "XFIXES selection owner update failed")) {
        return false;
    }
    const auto *event = server.next_event(observer);
    const auto *selection_notify = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::XFixesSelectionNotifyEvent>(event);
    if (!expect(selection_notify != nullptr &&
                    selection_notify->subtype == 0 &&
                    selection_notify->window == xmin::server::root_window_id &&
                    selection_notify->owner == parent_id &&
                    selection_notify->selection == selection &&
                    selection_notify->sequence == 31,
                "XFIXES selection notification lost typed state")) {
        return false;
    }
    server.pop_event(observer);

    auto source = std::make_shared<xmin::server::CursorImage>();
    auto target = std::make_shared<xmin::server::CursorImage>();
    source->name = server.atoms().intern("source-cursor");
    target->name = server.atoms().intern("target-cursor");
    source->width = target->width = 1;
    source->height = target->height = 1;
    source->pixels = {0xff112233U};
    target->pixels = {0xff445566U};
    if (!expect(server.add_cursor({source_cursor_id, source}, owner) &&
                    server.add_cursor({target_cursor_id, target}, owner) &&
                    source->serial != 0 && target->serial != 0 &&
                    source->serial != target->serial,
                "XFIXES cursor insertion did not allocate serials") ||
        !expect(server.select_xfixes_cursor_input(
                    observer, xmin::server::root_window_id, 1) ==
                    xmin::server::XFixesUpdate::updated,
                "XFIXES cursor subscription failed")) {
        return false;
    }
    server.window(xmin::server::root_window_id)->cursor = target;
    if (!expect(server.cursor_maybe_changed() ==
                    xmin::server::EventDelivery::delivered,
                "XFIXES initial displayed cursor was not reported")) {
        return false;
    }
    event = server.next_event(observer);
    auto *cursor_notify = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::XFixesCursorNotifyEvent>(event);
    if (!expect(cursor_notify != nullptr &&
                    cursor_notify->cursor_serial == target->serial &&
                    cursor_notify->name == target->name &&
                    cursor_notify->sequence == 31,
                "XFIXES cursor notification lost typed state")) {
        return false;
    }
    server.pop_event(observer);
    for (std::size_t count = 0;
         count < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.broadcast_mapping_notify(1, 96, 1),
                    "XFIXES cursor queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.replace_cursor(source, target) ==
                    xmin::server::XFixesUpdate::queue_full &&
                    server.current_cursor() == target,
                "failed XFIXES cursor notification partially replaced state")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);
    while (server.has_pending_event(observer))
        server.pop_event(observer);
    if (!expect(server.replace_cursor(source, target) ==
                    xmin::server::XFixesUpdate::updated &&
                    server.current_cursor() == source,
                "XFIXES cursor replacement missed a live reference")) {
        return false;
    }
    event = server.next_event(observer);
    cursor_notify = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::XFixesCursorNotifyEvent>(event);
    if (!expect(cursor_notify != nullptr &&
                    cursor_notify->cursor_serial == source->serial,
                "XFIXES cursor replacement omitted CursorNotify")) {
        return false;
    }
    server.pop_event(observer);
    if (!expect(server.hide_cursor(owner) ==
                    xmin::server::XFixesUpdate::updated &&
                    server.hide_cursor(owner) ==
                    xmin::server::XFixesUpdate::updated &&
                    server.cursor_hidden(),
                "nested XFIXES cursor hiding failed") ||
        !expect(server.show_cursor(owner) ==
                    xmin::server::XFixesUpdate::updated &&
                    server.cursor_hidden() &&
                    server.show_cursor(owner) ==
                    xmin::server::XFixesUpdate::updated &&
                    !server.cursor_hidden() &&
                    server.show_cursor(owner) ==
                    xmin::server::XFixesUpdate::invalid,
                "nested XFIXES cursor showing failed")) {
        return false;
    }

    if (!expect(server.inject_input(6, 0, 20, 24) ==
                    xmin::server::EventDelivery::no_recipient,
                "XFIXES barrier pointer setup delivered an event")) {
        return false;
    }
    xmin::server::XFixesBarrierRecord barrier;
    barrier.id = barrier_id;
    barrier.window = xmin::server::root_window_id;
    barrier.x1 = 30;
    barrier.x2 = 30;
    barrier.y2 = 48;
    if (!expect(server.add_xfixes_barrier(std::move(barrier), owner),
                "XFIXES pointer barrier insertion failed") ||
        !expect(server.inject_input(6, 0, 40, 24) ==
                    xmin::server::EventDelivery::no_recipient &&
                    server.input().pointer_x == 29 &&
                    server.input().pointer_y == 24,
                "XFIXES pointer barrier did not constrain motion")) {
        return false;
    }

    server.disconnect_client(owner);
    event = server.next_event(observer);
    selection_notify = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::XFixesSelectionNotifyEvent>(event);
    const auto *saved_child = server.window(child_id);
    const bool passed =
        expect(saved_child != nullptr &&
                   saved_child->parent == xmin::server::root_window_id &&
                   saved_child->mapped,
               "XFIXES save set did not rescue and map a foreign window") &&
        expect(server.window(parent_id) == nullptr,
               "disconnect retained the save-set owner window") &&
        expect(server.xfixes_region(region_id) == nullptr &&
                   !server.resource_exists(barrier_id) &&
                   server.cursor(source_cursor_id) == nullptr &&
                   server.cursor(target_cursor_id) == nullptr,
               "disconnect retained typed XFIXES resources") &&
        expect(selection_notify != nullptr &&
                   selection_notify->subtype == 2 &&
                   selection_notify->owner == 0 &&
                   selection_notify->selection == selection,
               "selection owner disconnect omitted XFIXES notification");
    server.disconnect_client(observer);
    return passed;
}

bool
test_randr_state()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t observer = 0x00400000;
    constexpr std::uint32_t root_picture = owner + 1;
    xmin::server::ServerState server(64, 48);
    if (!expect(server.register_client(owner) &&
                    server.register_client(observer),
                "RANDR client registration failed")) {
        return false;
    }
    server.note_client_sequence(observer, 41);
    if (!expect(server.add_render_picture(
                    {root_picture, xmin::server::render_xrgb32_format,
                     xmin::server::RenderDrawableSource{
                         xmin::server::root_window_id}, {}},
                    owner),
                "RANDR root picture setup failed")) {
        return false;
    }
    if (!expect(server.select_randr_input(
                    observer, xmin::server::root_window_id,
                    1U | 2U | 4U | 8U | 64U) ==
                    xmin::server::RandrUpdate::updated,
                "RANDR event subscription failed")) {
        return false;
    }
    const auto *event = server.next_event(observer);
    const auto *screen = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::RandrScreenChangeNotifyEvent>(event);
    if (!expect(screen != nullptr && screen->width == 64 &&
                    screen->height == 48 && screen->sequence == 41,
                "RANDR initial screen notification lost typed state")) {
        return false;
    }
    server.pop_event(observer);

    const auto property = server.atoms().intern("XMIN_RANDR_PROPERTY");
    auto candidate = server.randr();
    candidate.output_properties[property].value =
        xmin::server::PropertyValue{19, 32, {1, 2, 3, 4}};
    if (!expect(server.commit_randr_state(
                    std::move(candidate), 8U, property, 0) ==
                    xmin::server::RandrUpdate::updated,
                "RANDR output property transaction failed")) {
        return false;
    }
    event = server.next_event(observer);
    const auto *notify = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::RandrNotifyEvent>(event);
    if (!expect(notify != nullptr && notify->subtype == 2 &&
                    notify->output == xmin::server::randr_output_id &&
                    notify->atom == property && notify->sequence == 41,
                "RANDR output property notification lost typed state")) {
        return false;
    }
    server.pop_event(observer);

    for (std::size_t count = 0;
         count < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.broadcast_mapping_notify(1, 96, 1),
                    "RANDR queue-pressure setup failed")) {
            return false;
        }
    }
    candidate = server.randr();
    candidate.primary_output = 0;
    if (!expect(server.commit_randr_state(
                    std::move(candidate), 64U) ==
                    xmin::server::RandrUpdate::queue_full &&
                    server.randr().primary_output ==
                        xmin::server::randr_output_id,
                "failed RANDR notification partially committed state")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);
    while (server.has_pending_event(observer))
        server.pop_event(observer);

    candidate = server.randr();
    candidate.millimetre_width = 21;
    candidate.millimetre_height = 16;
    if (!expect(server.resize_randr_screen(
                    std::move(candidate), 80, 60) ==
                    xmin::server::RandrUpdate::updated &&
                    server.width() == 80 && server.height() == 60 &&
                    server.window(xmin::server::root_window_id)->surface->width() ==
                        80 &&
                    server.window(xmin::server::root_window_id)->surface->height() ==
                        60 &&
                    server.valid(),
                "RANDR framebuffer resize was not atomic")) {
        return false;
    }
    const auto *resized_picture = server.render_picture(root_picture);
    const auto *resized_drawable = resized_picture == nullptr
        ? nullptr
        : std::get_if<xmin::server::RenderDrawableSource>(
              &resized_picture->source);
    if (!expect(resized_drawable != nullptr &&
                    resized_drawable->surface ==
                        server.window(xmin::server::root_window_id)->surface &&
                    resized_drawable->surface->width() == 80 &&
                    resized_drawable->surface->height() == 60,
                "RANDR resize retained a stale root RENDER surface")) {
        return false;
    }
    event = server.next_event(observer);
    screen = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::RandrScreenChangeNotifyEvent>(event);
    if (!expect(screen != nullptr && screen->width == 80 &&
                    screen->height == 60 && screen->millimetre_width == 21,
                "RANDR resize omitted ScreenChangeNotify")) {
        return false;
    }
    while (server.has_pending_event(observer))
        server.pop_event(observer);

    server.disconnect_client(observer);
    candidate = server.randr();
    candidate.primary_output = 0;
    return expect(server.commit_randr_state(
                      std::move(candidate), 64U) ==
                      xmin::server::RandrUpdate::updated &&
                      !server.has_pending_event(observer),
                  "RANDR disconnect retained a stale subscription");
}

bool
test_damage_state()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t pixmap_id = owner + 1;
    constexpr std::uint32_t alias_id = owner + 2;
    constexpr std::uint32_t damage_id = owner + 3;
    constexpr std::uint32_t window_id = owner + 4;
    constexpr std::uint32_t window_damage_id = owner + 5;
    constexpr std::uint32_t bounding_damage_id = owner + 6;
    xmin::server::ServerState server(32, 24);
    if (!expect(server.register_client(owner),
                "DAMAGE client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 29);

    auto created = xmin::server::Surface::create(12, 10, 24);
    if (!expect(created.has_value(), "DAMAGE surface allocation failed"))
        return false;
    auto shared = server.adopt_surface(std::move(*created));
    if (!expect(shared != nullptr &&
                    server.add_pixmap({pixmap_id, shared}, owner) &&
                    server.add_pixmap({alias_id, shared}, owner),
                "DAMAGE shared drawable insertion failed") ||
        !expect(server.add_damage(
                    {damage_id, owner, pixmap_id, 3, {}}, owner) ==
                    xmin::server::DamageUpdate::updated,
                "DAMAGE object insertion failed") ||
        !expect(!server.has_pending_event(owner),
                "pixmap DAMAGE object emitted initial damage")) {
        return false;
    }

    xmin::server::Region changed;
    const std::vector<xmin::server::Rectangle> rectangles{{2, 3, 5, 4}};
    if (!expect(xmin::server::Region::canonicalize(rectangles, changed),
                "DAMAGE region canonicalization failed") ||
        !expect(server.damage_drawable(alias_id, &changed) ==
                    xmin::server::DamageUpdate::updated,
                "DAMAGE did not follow shared surface identity")) {
        return false;
    }
    const auto *event = server.next_event(owner);
    const auto *notify = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::DamageNotifyEvent>(event);
    if (!expect(notify != nullptr && notify->sequence == 29 &&
                    notify->drawable == pixmap_id &&
                    notify->damage == damage_id && notify->level == 3 &&
                    notify->area_x == 0 && notify->area_y == 0 &&
                    notify->area_width == 12 &&
                    notify->area_height == 10 &&
                    notify->geometry_width == 12 &&
                    notify->geometry_height == 10,
                "DAMAGE notification lost typed state")) {
        return false;
    }
    server.pop_event(owner);

    xmin::server::Region parts;
    if (!expect(server.subtract_damage(damage_id, nullptr, &parts) ==
                    xmin::server::DamageUpdate::updated &&
                    parts.rectangles().size() == 1 &&
                    parts.extents().x == 2 && parts.extents().y == 3 &&
                    parts.extents().width == 5 &&
                    parts.extents().height == 4,
                "DAMAGE Subtract did not transfer accumulated state")) {
        return false;
    }

    for (std::size_t count = 0;
         count < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.broadcast_mapping_notify(1, 96, 1),
                    "DAMAGE queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.damage_drawable(pixmap_id, &changed) ==
                    xmin::server::DamageUpdate::queue_full,
                "full event queue accepted a DAMAGE update") ||
        !expect(server.damage(damage_id) != nullptr &&
                    server.damage(damage_id)->accumulated.empty(),
                "failed DAMAGE notification partially committed state")) {
        return false;
    }
    while (server.has_pending_event(owner))
        server.pop_event(owner);

    xmin::server::Region disjoint;
    const std::vector<xmin::server::Rectangle> disjoint_rectangles{
        {0, 0, 2, 2}, {8, 7, 2, 2}};
    xmin::server::Region repair;
    const std::vector<xmin::server::Rectangle> repair_rectangles{
        {0, 0, 1, 1}};
    if (!expect(server.erase_damage(damage_id),
                "DAMAGE object cleanup failed") ||
        !expect(server.add_damage(
                    {bounding_damage_id, owner, pixmap_id, 2, {}}, owner) ==
                    xmin::server::DamageUpdate::updated &&
                    xmin::server::Region::canonicalize(
                        disjoint_rectangles, disjoint) &&
                    server.damage_drawable(pixmap_id, &disjoint) ==
                        xmin::server::DamageUpdate::updated,
                "bounding-box DAMAGE setup failed")) {
        return false;
    }
    server.pop_event(owner);
    if (!expect(xmin::server::Region::canonicalize(
                    repair_rectangles, repair) &&
                    server.subtract_damage(
                        bounding_damage_id, &repair, nullptr) ==
                        xmin::server::DamageUpdate::updated,
                "bounding-box DAMAGE subtraction failed")) {
        return false;
    }
    event = server.next_event(owner);
    notify = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::DamageNotifyEvent>(event);
    if (!expect(notify != nullptr && notify->level == 2 &&
                    (notify->level & 0x80U) == 0 &&
                    notify->area_x == 0 && notify->area_y == 0 &&
                    notify->area_width == 10 && notify->area_height == 9,
                "DAMAGE Subtract split a bounding-box notification")) {
        return false;
    }
    server.pop_event(owner);

    xmin::server::WindowRecord window;
    window.id = window_id;
    window.parent = xmin::server::root_window_id;
    window.width = 7;
    window.height = 6;
    auto window_surface = xmin::server::Surface::create(7, 6, 24);
    if (!expect(window_surface.has_value(),
                "DAMAGE window surface allocation failed")) {
        return false;
    }
    window.surface = server.adopt_surface(std::move(*window_surface));
    if (!expect(window.surface != nullptr &&
                    server.add_window(std::move(window), owner),
                "DAMAGE window insertion failed") ||
        !expect(server.add_damage(
                    {window_damage_id, owner, window_id, 2, {}}, owner) ==
                    xmin::server::DamageUpdate::updated,
                "window DAMAGE object insertion failed")) {
        return false;
    }
    event = server.next_event(owner);
    notify = event == nullptr
        ? nullptr
        : std::get_if<xmin::server::DamageNotifyEvent>(event);
    if (!expect(notify != nullptr && notify->drawable == window_id &&
                    notify->area_width == 7 && notify->area_height == 6,
                "window DAMAGE object omitted initial full damage")) {
        return false;
    }
    server.pop_event(owner);
    if (!expect(server.destroy_window(window_id) ==
                    xmin::server::EventDelivery::no_recipient &&
                    server.damage(window_damage_id) == nullptr,
                "drawable destruction retained its DAMAGE object")) {
        return false;
    }

    server.disconnect_client(owner);
    return expect(server.damage(damage_id) == nullptr &&
                      server.pixmap(pixmap_id) == nullptr &&
                      server.pixmap(alias_id) == nullptr,
                  "DAMAGE resources survived owner disconnect");
}

bool
test_composite_state()
{
    constexpr std::uint32_t redirector = 0x00200000;
    constexpr std::uint32_t owner = 0x00400000;
    constexpr std::uint32_t window_id = owner + 1;
    constexpr std::uint32_t pixmap_id = redirector + 1;
    constexpr std::uint32_t picture_id = owner + 2;
    xmin::server::ServerState server(16, 12);
    if (!expect(server.register_client(redirector) &&
                    server.register_client(owner),
                "Composite client registration failed")) {
        return false;
    }
    auto *root = server.window(xmin::server::root_window_id);
    root->surface->fill(
        {0, 0, 16, 12}, 0x000000ffU, 3, 0xffffffffU);

    auto created = xmin::server::Surface::create(4, 3, 24);
    if (!expect(created.has_value(),
                "Composite window surface allocation failed")) {
        return false;
    }
    created->fill({0, 0, 4, 3}, 0x00ff0000U, 3, 0xffffffffU);
    xmin::server::WindowRecord window;
    window.id = window_id;
    window.parent = xmin::server::root_window_id;
    window.x = 2;
    window.y = 1;
    window.width = 4;
    window.height = 3;
    window.mapped = true;
    window.surface = server.adopt_surface(std::move(*created));
    if (!expect(window.surface != nullptr &&
                    server.add_window(std::move(window), owner),
                "Composite window insertion failed") ||
        !expect(server.readable_surface(xmin::server::root_window_id)->
                        pixel(2, 1) == 0x00ff0000U,
                "unredirected window was absent from the scene")) {
        return false;
    }

    if (!expect(server.redirect_window(
                    redirector, window_id, false, 1) ==
                    xmin::server::CompositeUpdate::updated &&
                    server.composite_window_manually_redirected(window_id),
                "manual Composite redirection failed") ||
        !expect(server.readable_surface(xmin::server::root_window_id)->
                        pixel(2, 1) == 0x000000ffU,
                "manual Composite redirection remained in the scene") ||
        !expect(server.redirect_window(
                    redirector, window_id, false, 0) ==
                    xmin::server::CompositeUpdate::updated,
                "automatic redirection did not coexist with manual state") ||
        !expect(server.redirect_window(owner, window_id, false, 1) ==
                    xmin::server::CompositeUpdate::access_denied,
                "second manual Composite redirect was accepted")) {
        return false;
    }

    if (!expect(server.name_window_pixmap(
                    window_id, pixmap_id, redirector) ==
                    xmin::server::CompositeUpdate::updated,
                "Composite named pixmap creation failed") ||
        !expect(server.add_render_picture(
                    {picture_id, xmin::server::render_xrgb32_format,
                     xmin::server::RenderDrawableSource{window_id}, {}},
                    owner),
                "Composite resize picture setup failed") ||
        !expect(server.unredirect_window(
                    redirector, window_id, false, 1) ==
                    xmin::server::CompositeUpdate::updated &&
                    !server.composite_window_manually_redirected(window_id),
                "manual Composite unredirect failed")) {
        return false;
    }

    auto *stored = server.window(window_id);
    const auto *named_before = server.pixmap(pixmap_id)->surface.get();
    if (!expect(server.resize_window_surface(*stored, 6, 5),
                "Composite copy-on-write resize failed")) {
        return false;
    }
    stored->width = 6;
    stored->height = 5;
    const auto *picture = server.render_picture(picture_id);
    const auto *drawable = picture == nullptr
        ? nullptr
        : std::get_if<xmin::server::RenderDrawableSource>(&picture->source);
    if (!expect(server.pixmap(pixmap_id)->surface.get() == named_before &&
                    server.pixmap(pixmap_id)->surface->width() == 4 &&
                    server.pixmap(pixmap_id)->surface->height() == 3 &&
                    server.pixmap(pixmap_id)->surface->pixel(0, 0) ==
                        0x00ff0000U,
                "window resize mutated its named Composite pixmap") ||
        !expect(stored->surface.get() != named_before &&
                    stored->surface->width() == 6 &&
                    stored->surface->height() == 5 &&
                    drawable != nullptr &&
                    drawable->surface == stored->surface,
                "window resize did not rebind its live RENDER picture")) {
        return false;
    }
    stored->surface->fill(
        {0, 0, 6, 5}, 0x0000ff00U, 3, 0xffffffffU);
    server.invalidate_scene();
    if (!expect(server.pixmap(pixmap_id)->surface->pixel(0, 0) ==
                    0x00ff0000U,
                "new window writes escaped into a named pixmap")) {
        return false;
    }

    if (!expect(server.redirect_window(
                    redirector, xmin::server::root_window_id, true, 1) ==
                    xmin::server::CompositeUpdate::updated &&
                    server.composite_window_manually_redirected(window_id),
                "Composite subwindow redirection failed") ||
        !expect(server.redirect_window(owner, window_id, false, 1) ==
                    xmin::server::CompositeUpdate::access_denied,
                "direct manual redirect bypassed parent subwindow state") ||
        !expect(server.unredirect_window(
                    redirector, xmin::server::root_window_id, true, 1) ==
                    xmin::server::CompositeUpdate::updated,
                "Composite subwindow unredirect failed") ||
        !expect(server.unredirect_window(
                    redirector, xmin::server::root_window_id, true, 1) ==
                    xmin::server::CompositeUpdate::invalid,
                "duplicate Composite unredirect succeeded")) {
        return false;
    }

    if (!expect(server.unredirect_window(
                    redirector, window_id, false, 0) ==
                    xmin::server::CompositeUpdate::updated &&
                    server.redirect_window(
                        redirector, window_id, false, 1) ==
                    xmin::server::CompositeUpdate::updated,
                "Composite disconnect setup failed")) {
        return false;
    }
    server.disconnect_client(redirector);
    return expect(!server.composite_window_redirected(window_id) &&
                      server.pixmap(pixmap_id) == nullptr &&
                      server.window(window_id) != nullptr &&
                      server.render_picture(picture_id) != nullptr &&
                      server.readable_surface(xmin::server::root_window_id)->
                              pixel(2, 1) == 0x0000ff00U,
                  "Composite redirect or named pixmap survived disconnect");
}

bool
test_present_state()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t window_id = owner + 1;
    constexpr std::uint32_t pixmap_id = owner + 2;
    constexpr std::uint32_t event_id = owner + 3;
    constexpr std::uint32_t wait_fence = owner + 4;
    constexpr std::uint32_t idle_fence = owner + 5;
    constexpr std::uint32_t root_event_id = owner + 6;
    ManualClock clock;
    xmin::server::ServerState server(16, 12, clock);
    if (!expect(server.register_client(owner),
                "Present client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 73);

    auto window_surface = xmin::server::Surface::create(4, 3, 24);
    auto pixmap_surface = xmin::server::Surface::create(4, 3, 24);
    if (!expect(window_surface.has_value() && pixmap_surface.has_value(),
                "Present surface allocation failed")) {
        return false;
    }
    window_surface->fill({0, 0, 4, 3}, 0x000000ffU, 3, 0xffffffffU);
    pixmap_surface->fill({0, 0, 4, 3}, 0x0000ff00U, 3, 0xffffffffU);
    xmin::server::WindowRecord window;
    window.id = window_id;
    window.parent = xmin::server::root_window_id;
    window.width = 4;
    window.height = 3;
    window.mapped = true;
    window.bit_gravity = 1;
    window.surface = server.adopt_surface(std::move(*window_surface));
    auto managed_pixmap = server.adopt_surface(std::move(*pixmap_surface));
    if (!expect(window.surface != nullptr && managed_pixmap != nullptr &&
                    server.add_window(std::move(window), owner) &&
                    server.add_pixmap({pixmap_id, managed_pixmap}, owner),
                "Present drawable insertion failed") ||
        !expect(server.select_present_input(
                    owner, event_id, window_id, 0x7) ==
                    xmin::server::PresentUpdate::updated,
                "Present event selection failed")) {
        return false;
    }

    xmin::server::Region update;
    if (!expect(xmin::server::Region::canonicalize(
                    {{0, 0, 1, 1}}, update),
                "Present update region creation failed")) {
        return false;
    }
    xmin::server::PresentOperation operation;
    operation.kind = xmin::server::PresentKind::pixmap;
    operation.owner = owner;
    operation.window = window_id;
    operation.pixmap = pixmap_id;
    operation.serial = 0x584d494eU;
    operation.pixmap_surface = managed_pixmap;
    operation.update = update;
    operation.x_off = 1;
    if (!expect(server.submit_present(std::move(operation)) ==
                    xmin::server::PresentUpdate::updated &&
                    server.timer_timeout_milliseconds() == 17 &&
                    server.window(window_id)->surface->pixel(1, 0) ==
                        0x000000ffU,
                "Present did not arm its first software vblank")) {
        return false;
    }
    clock.advance(std::chrono::milliseconds{16});
    if (!expect(server.process_timers() ==
                    xmin::server::EventDelivery::no_recipient &&
                    server.timer_timeout_milliseconds() == 1,
                "Present fired before its software vblank")) {
        return false;
    }
    clock.advance(std::chrono::milliseconds{1});
    if (!expect(server.process_timers() ==
                    xmin::server::EventDelivery::delivered &&
                    server.window(window_id)->surface->pixel(1, 0) ==
                        0x0000ff00U &&
                    server.window(window_id)->surface->pixel(0, 0) ==
                        0x000000ffU,
                "Present region copy did not execute at vblank")) {
        return false;
    }
    const auto *queued = server.next_event(owner);
    const auto *complete = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::PresentCompleteNotifyEvent>(queued);
    if (!expect(complete != nullptr && complete->event == event_id &&
                    complete->window == window_id && complete->kind == 0 &&
                    complete->mode == 0 &&
                    complete->serial == 0x584d494eU && complete->msc == 1 &&
                    complete->ust > 0 && complete->sequence == 73,
                "Present CompleteNotify state is malformed")) {
        return false;
    }
    server.pop_event(owner);
    queued = server.next_event(owner);
    const auto *idle = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::PresentIdleNotifyEvent>(queued);
    if (!expect(idle != nullptr && idle->event == event_id &&
                    idle->serial == 0x584d494eU &&
                    idle->pixmap == pixmap_id && idle->idle_fence == 0,
                "Present IdleNotify state is malformed")) {
        return false;
    }
    server.pop_event(owner);

    auto *configured_window = server.window(window_id);
    if (!expect(configured_window != nullptr &&
                    server.configure_window(
                        *configured_window, 2, 1, 5, 4, 1,
                        std::nullopt, std::nullopt) ==
                    xmin::server::EventDelivery::delivered,
                "transactional ConfigureWindow failed")) {
        return false;
    }
    queued = server.next_event(owner);
    const auto *configure = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::PresentConfigureNotifyEvent>(queued);
    if (!expect(configure != nullptr && configure->event == event_id &&
                    configure->window == window_id &&
                    configure->x == 2 && configure->y == 1 &&
                    configure->width == 5 && configure->height == 4 &&
                    configure->pixmap_width == 5 &&
                    configure->pixmap_height == 4 &&
                    configured_window->surface->pixel(1, 0) ==
                        0x0000ff00U,
                "Present ConfigureNotify state is malformed")) {
        return false;
    }
    server.pop_event(owner);

    const auto *preserved_surface = configured_window->surface.get();
    for (std::size_t count = 0;
         count < xmin::server::maximum_pending_events_per_client; ++count) {
        if (!expect(server.present_window_configured(window_id) ==
                        xmin::server::EventDelivery::delivered,
                    "Present queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.configure_window(
                    *configured_window, 7, 8, 6, 5, 2,
                    std::nullopt, std::nullopt) ==
                    xmin::server::EventDelivery::queue_full &&
                    configured_window->x == 2 &&
                    configured_window->y == 1 &&
                    configured_window->width == 5 &&
                    configured_window->height == 4 &&
                    configured_window->border_width == 1 &&
                    configured_window->surface.get() == preserved_surface,
                "failed ConfigureWindow partially mutated state")) {
        return false;
    }
    for (std::size_t count = 0;
         count < xmin::server::maximum_pending_events_per_client; ++count) {
        server.pop_event(owner);
    }

    if (!expect(server.add_sync_fence({wait_fence, false}, owner) &&
                    server.add_sync_fence({idle_fence, false}, owner),
                "Present fence setup failed")) {
        return false;
    }
    xmin::server::PresentOperation fenced;
    fenced.kind = xmin::server::PresentKind::pixmap;
    fenced.owner = owner;
    fenced.window = window_id;
    fenced.pixmap = pixmap_id;
    fenced.serial = 2;
    fenced.pixmap_surface = managed_pixmap;
    fenced.wait_fence = wait_fence;
    fenced.idle_fence = idle_fence;
    fenced.options = 1; // asynchronous: current MSC once the fence opens
    if (!expect(server.submit_present(std::move(fenced)) ==
                    xmin::server::PresentUpdate::updated &&
                    server.timer_timeout_milliseconds() == -1,
                "Present wait fence created a busy deadline") ||
        !expect(server.trigger_sync_fence(wait_fence) ==
                    xmin::server::SyncUpdate::updated &&
                    server.timer_timeout_milliseconds() == 0 &&
                    server.process_timers() ==
                        xmin::server::EventDelivery::delivered &&
                    server.sync_fence(idle_fence)->triggered,
                "Present fences did not gate and release the pixmap")) {
        return false;
    }
    server.pop_event(owner);
    server.pop_event(owner);

    xmin::server::PresentOperation msc;
    msc.kind = xmin::server::PresentKind::notify_msc;
    msc.owner = owner;
    msc.window = window_id;
    msc.serial = 3;
    msc.target_msc = server.present_msc() + 1;
    if (!expect(server.submit_present(std::move(msc)) ==
                    xmin::server::PresentUpdate::updated,
                "Present NotifyMSC submission failed")) {
        return false;
    }
    clock.advance(std::chrono::milliseconds{17});
    if (!expect(server.process_timers() ==
                    xmin::server::EventDelivery::delivered,
                "Present NotifyMSC deadline did not fire")) {
        return false;
    }
    queued = server.next_event(owner);
    complete = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::PresentCompleteNotifyEvent>(queued);
    if (!expect(complete != nullptr && complete->kind == 1 &&
                    complete->serial == 3,
                "Present NotifyMSC completion is malformed")) {
        return false;
    }
    server.pop_event(owner);

    if (!expect(server.select_present_input(
                    owner, root_event_id, xmin::server::root_window_id, 1) ==
                    xmin::server::PresentUpdate::updated,
                "Present root event selection failed")) {
        return false;
    }
    auto randr = server.randr();
    if (!expect(server.resize_randr_screen(std::move(randr), 18, 14) ==
                    xmin::server::RandrUpdate::updated,
                "RANDR resize with Present selection failed")) {
        return false;
    }
    queued = server.next_event(owner);
    configure = queued == nullptr
        ? nullptr
        : std::get_if<xmin::server::PresentConfigureNotifyEvent>(queued);
    if (!expect(configure != nullptr && configure->event == root_event_id &&
                    configure->window == xmin::server::root_window_id &&
                    configure->width == 18 && configure->height == 14,
                "RANDR resize omitted Present ConfigureNotify")) {
        return false;
    }
    server.pop_event(owner);

    if (!expect(server.reset_sync_fence(idle_fence),
                "Present idle fence reset failed")) {
        return false;
    }
    xmin::server::PresentOperation canceled;
    canceled.kind = xmin::server::PresentKind::pixmap;
    canceled.owner = owner;
    canceled.window = window_id;
    canceled.pixmap = pixmap_id;
    canceled.serial = 4;
    canceled.pixmap_surface = managed_pixmap;
    canceled.idle_fence = idle_fence;
    canceled.target_msc = server.present_msc() + 10;
    if (!expect(server.submit_present(std::move(canceled)) ==
                    xmin::server::PresentUpdate::updated &&
                    !server.sync_fence(idle_fence)->triggered &&
                    server.destroy_window(window_id) !=
                        xmin::server::EventDelivery::queue_full &&
                    server.sync_fence(idle_fence)->triggered,
                "destroying a Present window did not release its pixmap")) {
        return false;
    }
    server.disconnect_client(owner);
    return expect(server.window(window_id) == nullptr &&
                      !server.has_pending_event(owner),
                  "Present state survived client disconnect");
}

bool
test_colormap_state()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t colormap = owner + 1;
    xmin::server::ServerState server(32, 24);
    if (!expect(server.colormap_exists(xmin::server::default_colormap_id),
                "default colormap is missing") ||
        !expect(server.add_colormap(colormap, owner),
                "client colormap insertion failed") ||
        !expect(!server.add_colormap(colormap, owner),
                "duplicate colormap insertion succeeded")) {
        return false;
    }
    server.install_colormap(colormap);
    if (!expect(server.installed_colormap() == colormap,
                "client colormap was not installed")) {
        return false;
    }
    server.disconnect_client(owner);
    return expect(!server.colormap_exists(colormap),
                  "disconnect retained a client colormap") &&
        expect(server.installed_colormap() ==
                   xmin::server::default_colormap_id,
               "disconnect did not restore the default colormap");
}

bool
test_shared_memory_state()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t segment = owner + 1;
    xmin::server::UniqueFd client_descriptor;
    auto created = xmin::server::SharedMemory::create(
        4096, false, client_descriptor);
    if (!expect(created && client_descriptor,
                "shared-memory RAII mapping creation failed")) {
        return false;
    }
    created.value().writable_data()[0] = std::byte{0x5a};
    xmin::server::ServerState server(32, 24);
    if (!expect(server.add_shared_memory(
                    segment, std::move(created.value()), owner),
                "shared-memory resource insertion failed") ||
        !expect(server.shared_memory(segment) != nullptr &&
                    server.shared_memory(segment)->data()[0] ==
                        std::byte{0x5a},
                "shared-memory resource did not retain its mapping") ||
        !expect(!server.add_shared_memory(
                    segment, xmin::server::SharedMemory{}, owner),
                "invalid shared-memory resource insertion succeeded")) {
        return false;
    }
    server.disconnect_client(owner);
    return expect(server.shared_memory(segment) == nullptr,
                  "disconnect retained a shared-memory mapping");
}

bool
test_result()
{
    const auto value = xmin::server::Result<int>::success(17);
    const auto error = xmin::server::Result<int>::failure(
        xmin::server::ErrorCode::malformed, "bad packet");
    return expect(value && value.value() == 17, "Result success failed") &&
        expect(!error && error.error().message == "bad packet",
               "Result failure failed");
}

} // namespace

int
main()
{
    return test_checked_arithmetic() && test_generated_core_protocol() &&
            test_wire_order(xmin::server::ByteOrder::little) &&
            test_wire_order(xmin::server::ByteOrder::big) &&
            test_property_byte_order() &&
            test_atoms_and_resources() && test_unique_fd() &&
            test_connection_event_reply_order() &&
            test_shared_server_state() && test_property_notifications() &&
            test_passive_grabs() &&
            test_input_routing() && test_key_repeat_timers() &&
            test_xkb_state() && test_xkb_detectable_repeat() &&
            test_xi2_state() &&
            test_crossing_events() &&
            test_automatic_pointer_grab() &&
            test_focus_events() &&
            test_structure_mapping_notifications() &&
            test_window_manager_redirects() &&
            test_resize_exposures() &&
            test_mapping_lifecycle_events() &&
            test_reparent_lifecycle_events() &&
            test_grab_transitions() &&
            test_disconnect_grab_transitions() &&
            test_pointer_grab_view_loss() &&
            test_keyboard_grab_view_loss() &&
            test_true_color() &&
            test_surface_raster_and_overlap() && test_region_clipping() &&
            test_render_engine() &&
            test_scene_composition() && test_shape_state() &&
            test_window_tree_mutations() && test_sync_state() &&
            test_xfixes_state() && test_randr_state() &&
            test_damage_state() &&
            test_composite_state() &&
            test_present_state() &&
            test_colormap_state() &&
            test_shared_memory_state() &&
            test_result()
        ? 0
        : 1;
}
