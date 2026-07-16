#ifndef XMIN_NEXT_ATOM_TABLE_HPP
#define XMIN_NEXT_ATOM_TABLE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xmin::next {

using AtomId = std::uint32_t;

class AtomTable {
public:
    AtomTable();

    AtomId intern(std::string_view name, bool only_if_exists = false);
    [[nodiscard]] std::optional<std::string_view> name(AtomId atom) const;
    [[nodiscard]] std::size_t size() const noexcept { return names_.size() - 1; }

private:
    std::vector<std::string> names_{std::string{}};
    std::unordered_map<std::string, AtomId> ids_;
};

} // namespace xmin::next

#endif
