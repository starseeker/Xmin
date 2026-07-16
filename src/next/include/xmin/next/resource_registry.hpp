#ifndef XMIN_NEXT_RESOURCE_REGISTRY_HPP
#define XMIN_NEXT_RESOURCE_REGISTRY_HPP

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace xmin::next {

enum class ResourceKind {
    window,
    pixmap,
    graphics_context,
    colormap,
};

struct ResourceRecord {
    ResourceKind kind;
    std::uint32_t owner;
};

class ResourceRegistry {
public:
    bool insert(std::uint32_t id, ResourceKind kind, std::uint32_t owner)
    {
        if (id == 0)
            return false;
        return resources_.emplace(id, ResourceRecord{kind, owner}).second;
    }

    [[nodiscard]] std::optional<ResourceRecord> find(std::uint32_t id) const
    {
        const auto found = resources_.find(id);
        if (found == resources_.end())
            return std::nullopt;
        return found->second;
    }

    [[nodiscard]] bool is(std::uint32_t id, ResourceKind kind) const
    {
        const auto record = find(id);
        return record && record->kind == kind;
    }

    std::size_t erase_owner(std::uint32_t owner)
    {
        std::size_t erased = 0;
        for (auto iterator = resources_.begin(); iterator != resources_.end();) {
            if (iterator->second.owner == owner) {
                iterator = resources_.erase(iterator);
                ++erased;
            }
            else {
                ++iterator;
            }
        }
        return erased;
    }

    [[nodiscard]] std::size_t size() const noexcept { return resources_.size(); }

private:
    std::unordered_map<std::uint32_t, ResourceRecord> resources_;
};

} // namespace xmin::next

#endif
