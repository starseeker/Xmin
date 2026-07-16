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
        !expect(server.valid_client_resource(first_owner, first_owner),
                "valid first client XID was rejected") ||
        !expect(!server.valid_client_resource(second_owner, first_owner),
                "foreign client XID range was accepted")) {
        return false;
    }

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
                    queued_message->data[0] == 0x584d494eU,
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
               "disconnect retained an event selection");
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
            test_shared_server_state() && test_true_color() &&
            test_surface_raster_and_overlap() && test_region_clipping() &&
            test_scene_composition() &&
            test_window_tree_mutations() && test_colormap_state() &&
            test_result()
        ? 0
        : 1;
}
