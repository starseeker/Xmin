#include "xmin/next/atom_table.hpp"
#include "xmin/next/checked.hpp"
#include "xmin/next/resource_registry.hpp"
#include "xmin/next/result.hpp"
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
    return test_checked_arithmetic() &&
            test_wire_order(xmin::next::ByteOrder::little) &&
            test_wire_order(xmin::next::ByteOrder::big) &&
            test_atoms_and_resources() && test_unique_fd() && test_result()
        ? 0
        : 1;
}
