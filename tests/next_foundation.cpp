#include "xmin/next/atom_table.hpp"
#include "xmin/next/checked.hpp"
#include "xmin/next/color.hpp"
#include "xmin/next/generated/core_protocol.hpp"
#include "xmin/next/resource_registry.hpp"
#include "xmin/next/result.hpp"
#include "xmin/next/server_state.hpp"
#include "xmin/next/surface.hpp"
#include "xmin/next/unique_fd.hpp"
#include "xmin/next/wire.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <string>
#include <variant>
#include <vector>
#include <unistd.h>

namespace {

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
    using xmin::next::checked_add;
    using xmin::next::checked_multiply;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    return expect(checked_add(std::size_t{2}, std::size_t{3}) == 5,
                  "checked_add rejected a valid sum") &&
        expect(!checked_add(maximum, std::size_t{1}),
               "checked_add accepted overflow") &&
        expect(checked_multiply(std::size_t{6}, std::size_t{7}) == 42,
               "checked_multiply rejected a valid product") &&
        expect(!checked_multiply(maximum, std::size_t{2}),
               "checked_multiply accepted overflow");
}

bool
test_generated_core_protocol()
{
    using xmin::next::CoreOpcode;
    const auto intern = static_cast<std::size_t>(CoreOpcode::InternAtom);
    return expect(xmin::next::core_request_table.size() == 128,
                  "generated core opcode table has the wrong size") &&
        expect(xmin::next::core_request_table[intern].name == "InternAtom",
               "generated core opcode name is wrong") &&
        expect(xmin::next::core_request_table[120].name == "Reserved" &&
                   !xmin::next::core_request_table[120].defined,
               "generated reserved opcode metadata is wrong");
}

bool
test_wire_order(xmin::next::ByteOrder order)
{
    xmin::next::WireWriter writer(order);
    writer.u8(0x12);
    writer.u16(0x3456);
    writer.u32(0x789abcde);
    writer.pad_to_four();

    xmin::next::WireReader reader(writer.data(), order);
    const auto byte = reader.u8();
    const auto word = reader.u16();
    const auto dword = reader.u32();
    return expect(byte == 0x12, "wire u8 round trip failed") &&
        expect(word == 0x3456, "wire u16 round trip failed") &&
        expect(dword == 0x789abcde, "wire u32 round trip failed") &&
        expect(reader.skip(reader.remaining()), "wire padding skip failed") &&
        expect(!reader.u8(), "wire reader crossed its bound");
}

bool
test_atoms_and_resources()
{
    xmin::next::AtomTable atoms;
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

    xmin::next::ResourceRegistry resources;
    return expect(resources.insert(1, xmin::next::ResourceKind::window, 0),
                  "resource insertion failed") &&
        expect(!resources.insert(1, xmin::next::ResourceKind::pixmap, 2),
               "duplicate resource insertion succeeded") &&
        expect(resources.insert(20, xmin::next::ResourceKind::pixmap, 7),
               "owned resource insertion failed") &&
        expect(resources.is(1, xmin::next::ResourceKind::window),
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
        xmin::next::UniqueFd first(descriptors[0]);
        xmin::next::UniqueFd second(std::move(first));
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
test_shared_server_state()
{
    constexpr std::uint32_t first_owner = 0x00200000;
    constexpr std::uint32_t second_owner = 0x00400000;
    xmin::next::ServerState server(320, 240);
    if (!expect(server.window(xmin::next::root_window_id) != nullptr,
                "server root window is missing") ||
        !expect(server.input().pointer_x == 160 &&
                    server.input().pointer_y == 120 &&
                    server.input().modifier_button_mask == 0 &&
                    server.input().pressed_buttons.none(),
                "input snapshot did not initialize at screen center") ||
        !expect(server.input().pressed_keys ==
                    std::array<std::uint8_t, 32>{},
                "input snapshot initialized with pressed keys") ||
        !expect(server.input().keymap_width ==
                    xmin::next::keysyms_per_keycode &&
                    server.input().keymap[
                        96 * server.input().keymap_width] == 0x0000ffc9U &&
                    server.input().modifier_map ==
                        std::vector<std::uint8_t>(
                            xmin::next::core_modifier_map.begin(),
                            xmin::next::core_modifier_map.end()) &&
                    server.input().auto_repeats ==
                        xmin::next::default_auto_repeats &&
                    server.input().pointer_map ==
                        xmin::next::default_pointer_map,
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
        : std::get_if<xmin::next::MappingNotifyEvent>(first_mapping);
    const auto *second_mapping_value = second_mapping == nullptr
        ? nullptr
        : std::get_if<xmin::next::MappingNotifyEvent>(second_mapping);
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

    xmin::next::WindowRecord parent;
    parent.id = first_owner;
    parent.parent = xmin::next::root_window_id;
    parent.width = 20;
    parent.height = 10;
    xmin::next::WindowRecord child;
    child.id = second_owner;
    child.parent = first_owner;
    child.width = 5;
    child.height = 4;
    if (!expect(server.add_window(std::move(parent), first_owner),
                "shared parent insertion failed") ||
        !expect(server.add_window(std::move(child), second_owner),
                "cross-client child insertion failed") ||
        !expect(server.window(xmin::next::root_window_id)->children.size() == 1,
                "root child relationship is wrong")) {
        return false;
    }

    auto *stored_parent = server.window(first_owner);
    auto *stored_child = server.window(second_owner);
    server.set_window_mapped(*stored_parent, true);
    server.set_window_mapped(*stored_child, true);
    server.advance_time();
    if (!expect(server.set_input_focus(
                    xmin::next::FocusKind::window, second_owner, 2, 0) ==
                    xmin::next::FocusUpdate::updated,
                "window focus update failed") ||
        !expect(server.set_input_focus(
                    xmin::next::FocusKind::none, 0, 0, 1) ==
                    xmin::next::FocusUpdate::ignored,
                "stale focus timestamp was accepted")) {
        return false;
    }
    server.set_window_mapped(*stored_child, false);
    if (!expect(server.input().focus.kind == xmin::next::FocusKind::window &&
                    server.input().focus.window == first_owner &&
                    server.input().focus.revert_to == 0,
                "parent focus reversion failed")) {
        return false;
    }
    server.set_window_mapped(*stored_child, true);
    if (!expect(server.set_input_focus(
                    xmin::next::FocusKind::window, second_owner, 1, 0) ==
                    xmin::next::FocusUpdate::updated,
                "pointer-root reversion setup failed")) {
        return false;
    }

    const auto selection = server.atoms().intern("XMIN_SELECTION");
    server.advance_time();
    if (!expect(server.set_selection_owner(
                    selection, first_owner, first_owner, 0) ==
                    xmin::next::SelectionUpdate::updated,
                "initial selection ownership failed") ||
        !expect(server.selection_owner(selection) == first_owner,
                "selection owner lookup failed")) {
        return false;
    }

    xmin::next::ClientMessageEvent message;
    message.format = 32;
    message.window = first_owner;
    message.type = selection;
    message.data[0] = 0x584d494eU;
    if (!expect(server.deliver_client_message(
                    first_owner, 0, false, message) ==
                    xmin::next::EventDelivery::delivered,
                "creator-targeted client event was not delivered")) {
        return false;
    }
    const auto *queued = server.next_event(first_owner);
    const auto *queued_message = queued == nullptr
        ? nullptr
        : std::get_if<xmin::next::ClientMessageEvent>(queued);
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
                    xmin::next::SelectionUpdate::updated,
                "selection ownership transfer failed")) {
        return false;
    }
    const auto *selection_event = server.next_event(first_owner);
    if (!expect(selection_event != nullptr &&
                    std::holds_alternative<xmin::next::SelectionClearEvent>(
                        *selection_event),
                "selection transfer did not notify the previous owner")) {
        return false;
    }
    server.pop_event(first_owner);
    server.window(xmin::next::root_window_id)
        ->event_masks.emplace(first_owner, 1);
    server.grab_server(first_owner);
    xmin::next::ActiveGrab active_grab;
    active_grab.owner = first_owner;
    active_grab.window = xmin::next::root_window_id;
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
        expect(server.window(xmin::next::root_window_id)->children.empty(),
               "root retained a destroyed child") &&
        expect(server.selection_owner(selection) == 0,
               "destroyed selection window remained the owner") &&
        expect(server.window(xmin::next::root_window_id)
                       ->event_masks.count(first_owner) == 0,
               "disconnect retained an event selection") &&
        expect(server.server_grab_owner() == 0,
               "disconnect retained a server grab") &&
        expect(server.input().focus.kind == xmin::next::FocusKind::pointer_root,
               "destroyed focus window did not revert to pointer root") &&
        expect(!server.input().pointer_grab &&
                   !server.input().keyboard_grab,
               "disconnect retained an active input grab");
}

bool
test_passive_grabs()
{
    constexpr std::uint32_t first_owner = 0x00200000;
    constexpr std::uint32_t second_owner = 0x00400000;
    constexpr std::uint8_t key = 38;
    constexpr std::uint8_t other_key = 39;
    constexpr std::uint16_t shift = 1;
    xmin::next::ServerState server(32, 24);

    xmin::next::PassiveGrab wildcard;
    wildcard.kind = xmin::next::PassiveGrabKind::key;
    wildcard.details = xmin::next::passive_grab_details(wildcard.kind, 0);
    wildcard.modifiers =
        xmin::next::passive_grab_modifiers(xmin::next::any_modifier);
    wildcard.owner = first_owner;
    wildcard.window = xmin::next::root_window_id;
    if (!expect(wildcard.details.count() == 248 &&
                    wildcard.modifiers.count() == 256,
                "passive wildcard domains have the wrong bounds") ||
        !expect(server.add_passive_grab(wildcard) ==
                    xmin::next::PassiveGrabUpdate::updated,
                "passive wildcard grab insertion failed")) {
        return false;
    }

    xmin::next::PassiveGrab exact = wildcard;
    exact.details = xmin::next::passive_grab_details(exact.kind, key);
    exact.modifiers = xmin::next::passive_grab_modifiers(shift);
    exact.owner = second_owner;
    if (!expect(server.add_passive_grab(exact) ==
                    xmin::next::PassiveGrabUpdate::access_denied,
                "overlapping cross-client passive grab was accepted") ||
        !expect(server.remove_passive_grab(
                    xmin::next::PassiveGrabKind::key, first_owner,
                    xmin::next::root_window_id, exact.details,
                    exact.modifiers) ==
                    xmin::next::PassiveGrabUpdate::updated,
                "exact passive wildcard subtraction failed") ||
        !expect(server.passive_grabs().size() == 2,
                "wildcard subtraction did not produce two rectangles") ||
        !expect(server.add_passive_grab(exact) ==
                    xmin::next::PassiveGrabUpdate::updated,
                "subtracted passive combination remained reserved")) {
        return false;
    }

    exact.details = xmin::next::passive_grab_details(exact.kind, other_key);
    if (!expect(server.add_passive_grab(exact) ==
                    xmin::next::PassiveGrabUpdate::access_denied,
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
        !expect(xmin::next::passive_grab_modifiers(0x0100).none(),
                "invalid passive modifier domain was materialized")) {
        return false;
    }

    xmin::next::WindowRecord confine;
    confine.id = first_owner;
    confine.parent = xmin::next::root_window_id;
    confine.width = 4;
    confine.height = 4;
    xmin::next::PassiveGrab confined;
    confined.kind = xmin::next::PassiveGrabKind::button;
    confined.details = xmin::next::passive_grab_details(confined.kind, 1);
    confined.modifiers = xmin::next::passive_grab_modifiers(0);
    confined.owner = first_owner;
    confined.window = xmin::next::root_window_id;
    confined.confine_to = first_owner;
    if (!expect(server.add_window(std::move(confine), first_owner),
                "passive-grab confine window insertion failed") ||
        !expect(server.add_passive_grab(std::move(confined)) ==
                    xmin::next::PassiveGrabUpdate::updated,
                "confined passive grab insertion failed")) {
        return false;
    }
    server.destroy_window(first_owner);
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
    xmin::next::ServerState server(100, 80);
    if (!expect(server.register_client(first_owner) &&
                    server.register_client(second_owner),
                "input-routing client registration failed")) {
        return false;
    }
    server.note_client_sequence(first_owner, 31);
    server.note_client_sequence(second_owner, 47);

    xmin::next::WindowRecord parent;
    parent.id = parent_id;
    parent.parent = xmin::next::root_window_id;
    parent.x = 10;
    parent.y = 8;
    parent.width = 60;
    parent.height = 50;
    xmin::next::WindowRecord child;
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
    server.set_window_mapped(*stored_parent, true);
    server.set_window_mapped(*stored_child, true);
    stored_parent->event_masks.emplace(
        second_owner, key_press_mask | key_release_mask);
    if (!expect(server.inject_input(6, 0, 18, 17) ==
                    xmin::next::EventDelivery::no_recipient &&
                    server.input().pointer_x == 18 &&
                    server.input().pointer_y == 17,
                "unselected motion did not update pointer state") ||
        !expect(server.set_input_focus(
                    xmin::next::FocusKind::window, parent_id, 0, 0) ==
                    xmin::next::FocusUpdate::updated,
                "input-routing focus setup failed") ||
        !expect(server.inject_input(2, 38, 18, 17) ==
                    xmin::next::EventDelivery::delivered,
                "key press did not propagate to an ancestor")) {
        return false;
    }
    const auto *queued = server.next_event(second_owner);
    const auto *key = queued == nullptr
        ? nullptr
        : std::get_if<xmin::next::CoreInputEvent>(queued);
    if (!expect(key != nullptr && key->type == 2 && key->detail == 38 &&
                    key->root == xmin::next::root_window_id &&
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
                    xmin::next::EventDelivery::no_recipient &&
                    !server.has_pending_event(second_owner) &&
                    (server.input().pressed_keys[38 >> 3] &
                     (1U << (38 & 7U))) == 0,
                "do-not-propagate did not stop a key release")) {
        return false;
    }
    stored_child->do_not_propagate_mask = 0;

    if (!expect(server.inject_input(6, 0, 90, 70) ==
                    xmin::next::EventDelivery::no_recipient &&
                    server.inject_input(2, 41, 90, 70) ==
                    xmin::next::EventDelivery::delivered,
                "focused key event outside the focus subtree was lost")) {
        return false;
    }
    queued = server.next_event(second_owner);
    key = queued == nullptr
        ? nullptr
        : std::get_if<xmin::next::CoreInputEvent>(queued);
    if (!expect(key != nullptr && key->event == parent_id &&
                    key->child == 0 && key->root_x == 90 &&
                    key->root_y == 70 && key->event_x == 80 &&
                    key->event_y == 62,
                "focused key event used the dispatch source as its child")) {
        return false;
    }
    server.pop_event(second_owner);
    if (!expect(server.inject_input(3, 41, 90, 70) ==
                    xmin::next::EventDelivery::delivered,
                "focused key release outside the focus subtree was lost")) {
        return false;
    }
    server.pop_event(second_owner);
    stored_parent->event_masks.erase(second_owner);
    server.window(xmin::next::root_window_id)->event_masks.emplace(
        first_owner, key_press_mask | key_release_mask);
    if (!expect(server.inject_input(2, 42, 90, 70) ==
                    xmin::next::EventDelivery::no_recipient &&
                    server.inject_input(3, 42, 90, 70) ==
                    xmin::next::EventDelivery::no_recipient &&
                    !server.has_pending_event(first_owner),
                "keyboard propagation escaped above the focus window")) {
        return false;
    }
    server.window(xmin::next::root_window_id)->event_masks.erase(first_owner);
    stored_parent->event_masks.emplace(
        second_owner, key_press_mask | key_release_mask);
    if (!expect(server.inject_input(6, 0, 18, 17) ==
                    xmin::next::EventDelivery::no_recipient,
                "pointer restoration unexpectedly delivered an event")) {
        return false;
    }

    server.input().keyboard_grab = xmin::next::ActiveGrab{
        first_owner, xmin::next::root_window_id, 0, server.current_time(),
        key_press_mask, 1, 1, false};
    if (!expect(server.inject_input(2, 39, 18, 17) ==
                    xmin::next::EventDelivery::delivered,
                "active keyboard grab did not receive a key press")) {
        return false;
    }
    queued = server.next_event(first_owner);
    key = queued == nullptr
        ? nullptr
        : std::get_if<xmin::next::CoreInputEvent>(queued);
    if (!expect(key != nullptr && key->detail == 39 &&
                    key->event == xmin::next::root_window_id &&
                    key->child == parent_id && key->sequence == 31 &&
                    !server.has_pending_event(second_owner),
                "active keyboard grab did not override normal selection")) {
        return false;
    }
    server.pop_event(first_owner);
    if (!expect(server.inject_input(3, 39, 18, 17) ==
                    xmin::next::EventDelivery::no_recipient,
                "unselected grabbed key release was delivered")) {
        return false;
    }
    server.input().keyboard_grab.reset();

    xmin::next::PassiveGrab passive;
    passive.kind = xmin::next::PassiveGrabKind::key;
    passive.details = xmin::next::passive_grab_details(passive.kind, 40);
    passive.modifiers = xmin::next::passive_grab_modifiers(0);
    passive.owner = first_owner;
    passive.window = parent_id;
    passive.event_mask = key_press_mask | key_release_mask;
    if (!expect(server.add_passive_grab(std::move(passive)) ==
                    xmin::next::PassiveGrabUpdate::updated,
                "passive input-routing grab insertion failed") ||
        !expect(server.inject_input(2, 40, 18, 17) ==
                    xmin::next::EventDelivery::delivered &&
                    server.input().keyboard_grab &&
                    server.input().keyboard_grab->passive &&
                    server.input().keyboard_grab->passive_detail == 40,
                "passive key grab did not activate")) {
        return false;
    }
    queued = server.next_event(first_owner);
    key = queued == nullptr
        ? nullptr
        : std::get_if<xmin::next::CoreInputEvent>(queued);
    if (!expect(key != nullptr && key->detail == 40 &&
                    key->event == parent_id && key->child == child_id,
                "passive grab press used the wrong event path")) {
        return false;
    }
    server.pop_event(first_owner);
    if (!expect(server.inject_input(3, 40, 18, 17) ==
                    xmin::next::EventDelivery::delivered &&
                    !server.input().keyboard_grab,
                "passive key grab did not release")) {
        return false;
    }
    queued = server.next_event(first_owner);
    key = queued == nullptr
        ? nullptr
        : std::get_if<xmin::next::CoreInputEvent>(queued);
    return expect(key != nullptr && key->type == 3 && key->detail == 40,
                  "passive grab release event was not delivered");
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
    xmin::next::ServerState server(100, 80);
    if (!expect(server.register_client(owner),
                "crossing client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 71);

    const auto add_window = [&](std::uint32_t id, std::uint32_t parent,
                                std::int16_t x, std::int16_t y,
                                std::uint16_t width,
                                std::uint16_t height) {
        xmin::next::WindowRecord window;
        window.id = id;
        window.parent = parent;
        window.x = x;
        window.y = y;
        window.width = width;
        window.height = height;
        if (!server.add_window(std::move(window), owner))
            return false;
        auto *stored = server.window(id);
        server.set_window_mapped(*stored, true);
        stored->event_masks.emplace(owner, crossing_mask);
        return true;
    };
    server.window(xmin::next::root_window_id)->event_masks.emplace(
        owner, crossing_mask);
    if (!expect(add_window(left, xmin::next::root_window_id,
                           0, 0, 40, 40) &&
                    add_window(left_child, left, 5, 5, 10, 10) &&
                    add_window(right, xmin::next::root_window_id,
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
            : std::get_if<xmin::next::CrossingEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->root ==
                xmin::next::root_window_id &&
            value->event == event && value->child == child &&
            value->event_x == event_x && value->event_y == event_y &&
            value->mode == 0 && value->same_screen && value->focus &&
            value->sequence == 71;
        server.pop_event(owner);
        return matches;
    };

    if (!expect(server.inject_input(6, 0, 7, 7) ==
                    xmin::next::EventDelivery::delivered,
                "descendant crossing was not delivered") ||
        !expect(crossing(8, 2, xmin::next::root_window_id, 0, 7, 7) &&
                    crossing(7, 1, left, left_child, 7, 7) &&
                    crossing(7, 0, left_child, 0, 2, 2) &&
                    !server.has_pending_event(owner),
                "descendant crossing path is wrong")) {
        return false;
    }

    if (!expect(server.inject_input(6, 0, 57, 7) ==
                    xmin::next::EventDelivery::delivered,
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
                    xmin::next::EventDelivery::delivered,
                "ancestor crossing was not delivered") ||
        !expect(crossing(8, 0, right_child, 0, 40, 65) &&
                    crossing(8, 1, right, right_child, 45, 70) &&
                    crossing(7, 2, xmin::next::root_window_id, 0, 95, 70) &&
                    !server.has_pending_event(owner),
                "ancestor crossing path is wrong")) {
        return false;
    }

    xmin::next::ClientMessageEvent message;
    message.window = left;
    for (std::size_t count = 0;
         count + 1 < xmin::next::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(left, 0, false, message) ==
                        xmin::next::EventDelivery::delivered,
                    "crossing queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.inject_input(6, 0, 7, 7) ==
                    xmin::next::EventDelivery::queue_full &&
                    server.input().pointer_x == 95 &&
                    server.input().pointer_y == 70,
                "partial crossing escaped an atomic queue failure")) {
        return false;
    }
    std::size_t queued = 0;
    while (server.has_pending_event(owner)) {
        const auto *event = server.next_event(owner);
        if (!expect(event != nullptr &&
                        std::holds_alternative<xmin::next::ClientMessageEvent>(
                            *event),
                    "queue failure left a partial crossing event")) {
            return false;
        }
        server.pop_event(owner);
        ++queued;
    }
    return expect(
        queued + 1 == xmin::next::maximum_pending_events_per_client,
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
    xmin::next::ServerState server(100, 80);
    if (!expect(server.register_client(first_owner) &&
                    server.register_client(second_owner),
                "automatic-grab client registration failed")) {
        return false;
    }

    xmin::next::WindowRecord parent_window;
    parent_window.id = parent;
    parent_window.parent = xmin::next::root_window_id;
    parent_window.x = 10;
    parent_window.y = 10;
    parent_window.width = 40;
    parent_window.height = 40;
    xmin::next::WindowRecord child_window;
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
    server.set_window_mapped(*stored_parent, true);
    server.set_window_mapped(*stored_child, true);
    if (!expect(server.inject_input(6, 0, 18, 18) ==
                    xmin::next::EventDelivery::no_recipient,
                "automatic-grab pointer setup delivered an event")) {
        return false;
    }
    stored_parent->event_masks.emplace(
        first_owner, button_masks | crossing_masks);
    stored_child->event_masks.emplace(second_owner, crossing_masks);
    if (!expect(server.inject_input(4, 1, 18, 18) ==
                    xmin::next::EventDelivery::delivered &&
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
        : std::get_if<xmin::next::CoreInputEvent>(queued);
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
        : std::get_if<xmin::next::CrossingEvent>(queued);
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
        : std::get_if<xmin::next::CrossingEvent>(queued);
    if (!expect(crossing != nullptr && crossing->type == 8 &&
                    crossing->detail == 0 && crossing->event == child &&
                    crossing->mode == 1,
                "automatic-grab leave transition is wrong")) {
        return false;
    }
    server.pop_event(second_owner);

    if (!expect(server.inject_input(4, 2, 18, 18) ==
                    xmin::next::EventDelivery::delivered &&
                    server.inject_input(5, 1, 18, 18) ==
                    xmin::next::EventDelivery::delivered &&
                    server.input().pointer_grab &&
                    server.input().pointer_grab->automatic,
                "automatic grab ended before the final button release")) {
        return false;
    }
    server.pop_event(first_owner);
    server.pop_event(first_owner);
    if (!expect(server.inject_input(5, 2, 18, 18) ==
                    xmin::next::EventDelivery::delivered &&
                    !server.input().pointer_grab &&
                    server.input().pressed_buttons.none(),
                "automatic grab survived the final button release")) {
        return false;
    }
    queued = server.next_event(first_owner);
    button = queued == nullptr
        ? nullptr
        : std::get_if<xmin::next::CoreInputEvent>(queued);
    if (!expect(button != nullptr && button->type == 5 &&
                    button->detail == 2 && button->state == (1U << 9),
                "final grabbed button release has the wrong state")) {
        return false;
    }
    server.pop_event(first_owner);
    queued = server.next_event(first_owner);
    crossing = queued == nullptr
        ? nullptr
        : std::get_if<xmin::next::CrossingEvent>(queued);
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
        : std::get_if<xmin::next::CrossingEvent>(queued);
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
    xmin::next::ServerState server(100, 80);
    if (!expect(server.register_client(owner),
                "focus-event client registration failed")) {
        return false;
    }
    server.note_client_sequence(owner, 83);
    const auto add_window = [&](std::uint32_t id, std::uint32_t parent_id,
                                std::int16_t x, std::int16_t y,
                                std::uint16_t width,
                                std::uint16_t height) {
        xmin::next::WindowRecord window;
        window.id = id;
        window.parent = parent_id;
        window.x = x;
        window.y = y;
        window.width = width;
        window.height = height;
        if (!server.add_window(std::move(window), owner))
            return false;
        server.set_window_mapped(*server.window(id), true);
        return true;
    };
    if (!expect(add_window(parent, xmin::next::root_window_id,
                           5, 5, 40, 40) &&
                    add_window(child, parent, 5, 5, 20, 20) &&
                    add_window(sibling, xmin::next::root_window_id,
                               60, 5, 30, 30) &&
                    server.inject_input(6, 0, 15, 15) ==
                        xmin::next::EventDelivery::no_recipient,
                "focus-event window setup failed")) {
        return false;
    }
    server.window(xmin::next::root_window_id)->event_masks.emplace(
        owner, focus_mask);
    server.window(parent)->event_masks.emplace(owner, focus_mask);
    server.window(child)->event_masks.emplace(owner, focus_mask);
    server.window(sibling)->event_masks.emplace(owner, focus_mask);

    const auto focus = [&](std::uint8_t type, std::uint8_t detail,
                           std::uint32_t event) {
        const auto *queued = server.next_event(owner);
        const auto *value = queued == nullptr
            ? nullptr
            : std::get_if<xmin::next::FocusEvent>(queued);
        const bool matches = value != nullptr && value->type == type &&
            value->detail == detail && value->event == event &&
            value->mode == 0 && value->sequence == 83;
        server.pop_event(owner);
        return matches;
    };
    if (!expect(server.set_input_focus(
                    xmin::next::FocusKind::window, child, 0, 0) ==
                    xmin::next::FocusUpdate::updated &&
                    focus(10, 6, xmin::next::root_window_id) &&
                    focus(9, 4, xmin::next::root_window_id) &&
                    focus(9, 4, parent) && focus(9, 3, child),
                "PointerRoot-to-window focus path is wrong")) {
        return false;
    }
    if (!expect(server.set_input_focus(
                    xmin::next::FocusKind::window, sibling, 0, 0) ==
                    xmin::next::FocusUpdate::updated &&
                    focus(10, 3, child) && focus(10, 4, parent) &&
                    focus(9, 3, sibling),
                "nonlinear focus path is wrong")) {
        return false;
    }
    if (!expect(server.set_input_focus(
                    xmin::next::FocusKind::window, child, 0, 0) ==
                    xmin::next::FocusUpdate::updated &&
                    focus(10, 3, sibling) && focus(9, 4, parent) &&
                    focus(9, 3, child),
                "reverse nonlinear focus path is wrong")) {
        return false;
    }
    if (!expect(server.set_input_focus(
                    xmin::next::FocusKind::window,
                    xmin::next::root_window_id, 0, 0) ==
                    xmin::next::FocusUpdate::updated &&
                    focus(10, 0, child) && focus(10, 1, parent) &&
                    focus(9, 2, xmin::next::root_window_id),
                "ancestor focus path is wrong")) {
        return false;
    }
    if (!expect(server.set_input_focus(
                    xmin::next::FocusKind::none, 0, 0, 0) ==
                    xmin::next::FocusUpdate::updated &&
                    focus(10, 3, xmin::next::root_window_id) &&
                    focus(9, 7, xmin::next::root_window_id) &&
                    server.set_input_focus(
                        xmin::next::FocusKind::pointer_root, 0, 0, 0) ==
                        xmin::next::FocusUpdate::updated &&
                    focus(10, 7, xmin::next::root_window_id) &&
                    focus(9, 6, xmin::next::root_window_id) &&
                    !server.has_pending_event(owner),
                "None/PointerRoot focus path is wrong")) {
        return false;
    }
    xmin::next::ClientMessageEvent message;
    message.window = sibling;
    for (std::size_t count = 0;
         count + 1 < xmin::next::maximum_pending_events_per_client; ++count) {
        if (!expect(server.deliver_client_message(
                        sibling, 0, false, message) ==
                        xmin::next::EventDelivery::delivered,
                    "focus queue-pressure setup failed")) {
            return false;
        }
    }
    if (!expect(server.set_input_focus(
                    xmin::next::FocusKind::window, child, 0, 0) ==
                    xmin::next::FocusUpdate::queue_full &&
                    server.input().focus.kind ==
                        xmin::next::FocusKind::pointer_root,
                "partial focus path escaped an atomic queue failure")) {
        return false;
    }
    std::size_t queued_count = 0;
    while (server.has_pending_event(owner)) {
        const auto *event = server.next_event(owner);
        if (!expect(event != nullptr &&
                        std::holds_alternative<xmin::next::ClientMessageEvent>(
                            *event),
                    "queue failure left a partial focus event")) {
            return false;
        }
        server.pop_event(owner);
        ++queued_count;
    }
    return expect(
        queued_count + 1 == xmin::next::maximum_pending_events_per_client,
        "focus queue failure changed the preexisting event count");
}

bool
test_true_color()
{
    const auto red = xmin::next::parse_color("Red");
    const auto compact = xmin::next::parse_color("#0f8");
    const auto precise = xmin::next::parse_color("#123456789abc");
    return expect(red && red->red == 0xffff && red->green == 0 &&
                      red->blue == 0,
                  "named color parsing failed") &&
        expect(compact && compact->red == 0 && compact->green == 0xffff &&
                      compact->blue == 0x8888,
               "compact hexadecimal color parsing failed") &&
        expect(precise && precise->red == 0x1234 &&
                      precise->green == 0x5678 && precise->blue == 0x9abc,
               "precise hexadecimal color parsing failed") &&
        expect(!xmin::next::parse_color("not-a-color"),
               "invalid color name was accepted") &&
        expect(xmin::next::true_color_pixel(*red) == 0x00ff0000U,
               "TrueColor pixel packing failed") &&
        expect(xmin::next::true_color_rgb(0x00123456U).green == 0x3434,
               "TrueColor pixel query failed");
}

bool
test_surface_raster_and_overlap()
{
    if (!expect(!xmin::next::Surface::create(65535, 65535, 24),
                "oversized surface was accepted")) {
        return false;
    }
    auto surface = xmin::next::Surface::create(4, 2, 24);
    if (!expect(surface.has_value(), "bounded surface creation failed"))
        return false;
    surface->fill(xmin::next::Rectangle{0, 0, 4, 2}, 0x00112233U, 3,
                  0xffffffffU);
    surface->fill(xmin::next::Rectangle{1, 0, 1, 1}, 0x00aabbccU, 3,
                  0x0000ff00U);
    if (!expect(surface->pixel(1, 0) == 0x0011bb33U,
                "surface plane mask was not applied")) {
        return false;
    }

    for (std::int16_t x = 0; x < 4; ++x) {
        surface->fill(xmin::next::Rectangle{x, 1, 1, 1},
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

    auto lines = xmin::next::Surface::create(8, 8, 24);
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

    auto bitmap = xmin::next::Surface::create(4, 1, 1);
    auto projected = xmin::next::Surface::create(4, 1, 24);
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
    xmin::next::Region region;
    const std::vector<xmin::next::Rectangle> overlapping = {
        {0, 0, 3, 2}, {2, 0, 3, 2}};
    if (!expect(xmin::next::Region::canonicalize(overlapping, region),
                "region canonicalization failed") ||
        !expect(region.contains(0, 0) && region.contains(4, 1) &&
                    !region.contains(5, 1),
                "canonical region bounds are wrong")) {
        return false;
    }

    auto surface = xmin::next::Surface::create(8, 2, 24);
    if (!expect(surface.has_value(), "clip-test surface creation failed"))
        return false;
    surface->fill({0, 0, 8, 2}, 0x00ff0000U, 3, 0xffffffffU);
    surface->fill({0, 0, 8, 2}, 0x00ffffffU, 6, 0xffffffffU,
                  xmin::next::ClipView{&region, 1, 0});
    if (!expect(surface->pixel(0, 0) == 0x00ff0000U &&
                    surface->pixel(1, 0) == 0x0000ffffU &&
                    surface->pixel(3, 0) == 0x0000ffffU &&
                    surface->pixel(5, 1) == 0x0000ffffU &&
                    surface->pixel(6, 1) == 0x00ff0000U,
                "canonical clip was not applied as a disjoint union")) {
        return false;
    }

    xmin::next::Region empty;
    const std::vector<xmin::next::Rectangle> no_rectangles;
    if (!expect(xmin::next::Region::canonicalize(no_rectangles, empty),
                "empty region canonicalization failed")) {
        return false;
    }
    surface->fill({0, 0, 8, 2}, 0x0000ff00U, 3, 0xffffffffU,
                  xmin::next::ClipView{&empty, 0, 0});
    return expect(surface->pixel(0, 0) == 0x00ff0000U &&
                      surface->pixel(1, 0) == 0x0000ffffU,
                  "empty clip region did not suppress drawing");
}

bool
test_scene_composition()
{
    constexpr std::uint32_t owner = 0x00200000;
    xmin::next::ServerState server(16, 12);
    auto *root = server.window(xmin::next::root_window_id);
    root->surface->fill({0, 0, 16, 12}, 0x000000ffU, 3, 0xffffffffU);
    server.invalidate_scene();

    auto parent_surface = xmin::next::Surface::create(8, 6, 24);
    auto child_surface = xmin::next::Surface::create(4, 4, 24);
    if (!expect(parent_surface && child_surface,
                "scene test surface allocation failed")) {
        return false;
    }
    parent_surface->fill({0, 0, 8, 6}, 0x0000ff00U, 3, 0xffffffffU);
    child_surface->fill({0, 0, 4, 4}, 0x00ffff00U, 3, 0xffffffffU);

    xmin::next::WindowRecord parent;
    parent.id = owner;
    parent.parent = xmin::next::root_window_id;
    parent.x = 2;
    parent.y = 1;
    parent.width = 8;
    parent.height = 6;
    parent.border_width = 1;
    parent.border_pixel = 0x00ff0000U;
    parent.mapped = true;
    parent.surface = std::move(*parent_surface);
    xmin::next::WindowRecord child;
    child.id = owner + 1;
    child.parent = owner;
    child.x = 6;
    child.y = 4;
    child.width = 4;
    child.height = 4;
    child.mapped = true;
    child.surface = std::move(*child_surface);
    if (!expect(server.add_window(std::move(parent), owner),
                "scene parent insertion failed") ||
        !expect(server.add_window(std::move(child), owner),
                "scene child insertion failed")) {
        return false;
    }

    const auto *composed = server.readable_surface(xmin::next::root_window_id);
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

    server.set_window_mapped(*server.window(owner), false);
    composed = server.readable_surface(xmin::next::root_window_id);
    if (!expect(composed->pixel(3, 2) == 0x000000ffU,
                "unmapped window remained in the scene")) {
        return false;
    }
    server.set_window_mapped(*server.window(owner), true);
    composed = server.readable_surface(xmin::next::root_window_id);
    return expect(composed->pixel(3, 2) == 0x0000ff00U,
                  "remapped window did not return to the scene");
}

bool
test_window_tree_mutations()
{
    constexpr std::uint32_t owner = 0x00200000;
    xmin::next::ServerState server(32, 24);
    xmin::next::WindowRecord parent;
    parent.id = owner;
    parent.parent = xmin::next::root_window_id;
    xmin::next::WindowRecord child;
    child.id = owner + 1;
    child.parent = owner;
    child.mapped = true;
    if (!expect(server.add_window(std::move(parent), owner),
                "tree parent insertion failed") ||
        !expect(server.add_window(std::move(child), owner),
                "tree child insertion failed")) {
        return false;
    }
    server.set_subwindows_mapped(owner, false);
    if (!expect(!server.window(owner + 1)->mapped,
                "UnmapSubwindows state transition failed")) {
        return false;
    }
    server.set_subwindows_mapped(owner, true);
    if (!expect(!server.reparent_window(owner, owner + 1, 0, 0),
                "window cycle was accepted") ||
        !expect(server.reparent_window(
                    owner + 1, xmin::next::root_window_id, 7, 8),
                "window reparenting failed") ||
        !expect(server.window(owner + 1)->parent ==
                    xmin::next::root_window_id,
                "reparented window retained its old parent") ||
        !expect(server.window(owner)->children.empty(),
                "old parent retained a reparented child") ||
        !expect(server.reparent_window(owner + 1, owner, 1, 2),
                "window reparenting back to its owner failed")) {
        return false;
    }
    server.destroy_subwindows(owner);
    return expect(server.window(owner + 1) == nullptr,
                  "DestroySubwindows retained a child") &&
        expect(server.window(owner) != nullptr,
               "DestroySubwindows removed its parent");
}

bool
test_colormap_state()
{
    constexpr std::uint32_t owner = 0x00200000;
    constexpr std::uint32_t colormap = owner + 1;
    xmin::next::ServerState server(32, 24);
    if (!expect(server.colormap_exists(xmin::next::default_colormap_id),
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
                   xmin::next::default_colormap_id,
               "disconnect did not restore the default colormap");
}

bool
test_result()
{
    const auto value = xmin::next::Result<int>::success(17);
    const auto error = xmin::next::Result<int>::failure(
        xmin::next::ErrorCode::malformed, "bad packet");
    return expect(value && value.value() == 17, "Result success failed") &&
        expect(!error && error.error().message == "bad packet",
               "Result failure failed");
}

} // namespace

int
main()
{
    return test_checked_arithmetic() && test_generated_core_protocol() &&
            test_wire_order(xmin::next::ByteOrder::little) &&
            test_wire_order(xmin::next::ByteOrder::big) &&
            test_atoms_and_resources() && test_unique_fd() &&
            test_shared_server_state() && test_passive_grabs() &&
            test_input_routing() &&
            test_crossing_events() &&
            test_automatic_pointer_grab() &&
            test_focus_events() &&
            test_true_color() &&
            test_surface_raster_and_overlap() && test_region_clipping() &&
            test_scene_composition() &&
            test_window_tree_mutations() && test_colormap_state() &&
            test_result()
        ? 0
        : 1;
}
