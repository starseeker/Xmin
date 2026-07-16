#include "xmin/next/server_state.hpp"

#include <algorithm>
#include <utility>

namespace xmin::next {

ServerState::ServerState(std::uint16_t width, std::uint16_t height)
    : width_(width), height_(height)
{
    WindowRecord root;
    root.id = root_window_id;
    root.width = width;
    root.height = height;
    root.mapped = true;
    windows_.emplace(root.id, std::move(root));
    static_cast<void>(
        resources_.insert(root_window_id, ResourceKind::window, 0));
    static_cast<void>(
        resources_.insert(default_colormap_id, ResourceKind::colormap, 0));
}

WindowRecord *
ServerState::window(std::uint32_t id)
{
    const auto found = windows_.find(id);
    return found == windows_.end() ? nullptr : &found->second;
}

const WindowRecord *
ServerState::window(std::uint32_t id) const
{
    const auto found = windows_.find(id);
    return found == windows_.end() ? nullptr : &found->second;
}

bool
ServerState::resource_exists(std::uint32_t id) const
{
    return resources_.find(id).has_value();
}

bool
ServerState::valid_client_resource(std::uint32_t id, std::uint32_t base) const
{
    return (id & ~client_resource_mask) == base && !resource_exists(id);
}

bool
ServerState::resource_limit_reached(std::uint32_t owner) const
{
    return resources_.size() >= maximum_server_resources ||
        resources_.owner_size(owner) >= maximum_client_resources;
}

bool
ServerState::add_window(WindowRecord added, std::uint32_t owner)
{
    if (window(added.parent) == nullptr || resource_exists(added.id))
        return false;
    const std::uint32_t parent_id = added.parent;
    const std::uint32_t id = added.id;
    if (!resources_.insert(id, ResourceKind::window, owner))
        return false;
    auto inserted = windows_.emplace(id, std::move(added));
    if (!inserted.second) {
        static_cast<void>(resources_.erase(id));
        return false;
    }
    window(parent_id)->children.push_back(id);
    return true;
}

void
ServerState::destroy_window(std::uint32_t id)
{
    if (id == root_window_id)
        return;
    auto found = windows_.find(id);
    if (found == windows_.end())
        return;

    const auto children = found->second.children;
    for (const auto child : children)
        destroy_window(child);

    const std::uint32_t parent_id = found->second.parent;
    if (auto *parent = window(parent_id)) {
        parent->children.erase(
            std::remove(parent->children.begin(), parent->children.end(), id),
            parent->children.end());
    }
    windows_.erase(found);
    static_cast<void>(resources_.erase(id));
}

void
ServerState::disconnect_client(std::uint32_t owner)
{
    const auto windows = resources_.owned_by(owner, ResourceKind::window);
    for (const auto id : windows)
        destroy_window(id);
    static_cast<void>(resources_.erase_owner(owner));
}

std::uint8_t
ServerState::map_state(std::uint32_t id) const
{
    const auto *candidate = window(id);
    if (candidate == nullptr || !candidate->mapped)
        return 0; // Unmapped
    while (candidate->parent != 0) {
        candidate = window(candidate->parent);
        if (candidate == nullptr || !candidate->mapped)
            return 1; // Unviewable
    }
    return 2; // Viewable
}

std::uint32_t
ServerState::all_event_masks(const WindowRecord &candidate) const
{
    std::uint32_t masks = 0;
    for (const auto &selection : candidate.event_masks)
        masks |= selection.second;
    return masks;
}

} // namespace xmin::next
