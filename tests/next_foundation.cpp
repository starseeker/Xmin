#include "xmin/next/atom_table.hpp"
#include "xmin/next/checked.hpp"
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
    server.disconnect_client(first_owner);
    return expect(server.window(first_owner) == nullptr,
                  "owner disconnect retained its window") &&
        expect(server.window(second_owner) == nullptr,
               "parent teardown retained a foreign child") &&
        expect(server.window(xmin::next::root_window_id)->children.empty(),
               "root retained a destroyed child");
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
    return expect(surface->pixel(0, 1) == 1 &&
                      surface->pixel(1, 1) == 1 &&
                      surface->pixel(2, 1) == 2 &&
                      surface->pixel(3, 1) == 3,
                  "overlapping surface copy did not use a snapshot");
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
            test_shared_server_state() && test_surface_raster_and_overlap() &&
            test_result()
        ? 0
        : 1;
}
