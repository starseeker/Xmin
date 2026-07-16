#include "xmin/next/server_state.hpp"

#include "xmin/next/checked.hpp"

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
    root.surface = Surface::create(width, height, 24);
    if (root.surface)
        surface_bytes_ = root.surface->storage_bytes();
    windows_.emplace(root.id, std::move(root));
    static_cast<void>(
        resources_.insert(root_window_id, ResourceKind::window, 0));
    static_cast<void>(
        resources_.insert(default_colormap_id, ResourceKind::colormap, 0));
}

bool
ServerState::valid() const noexcept
{
    const auto *root = window(root_window_id);
    return root != nullptr && root->surface.has_value();
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
    const std::size_t added_bytes = added.surface
        ? added.surface->storage_bytes()
        : 0;
    if (surface_bytes_ > maximum_server_surface_bytes - added_bytes)
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
    surface_bytes_ += added_bytes;
    return true;
}

bool
ServerState::resize_window_surface(WindowRecord &candidate,
                                   std::uint16_t width,
                                   std::uint16_t height)
{
    if (!candidate.surface)
        return true;
    const auto pixels = checked_multiply(static_cast<std::size_t>(width),
                                         static_cast<std::size_t>(height));
    const auto bytes = pixels
        ? checked_multiply(*pixels, sizeof(std::uint32_t))
        : std::optional<std::size_t>{};
    if (!bytes || *bytes > maximum_surface_bytes)
        return false;
    const std::size_t old_bytes = candidate.surface->storage_bytes();
    if (surface_bytes_ - old_bytes > maximum_server_surface_bytes - *bytes)
        return false;
    if (!candidate.surface->resize(width, height))
        return false;
    surface_bytes_ = surface_bytes_ - old_bytes + *bytes;
    return true;
}

PixmapRecord *
ServerState::pixmap(std::uint32_t id)
{
    const auto found = pixmaps_.find(id);
    return found == pixmaps_.end() ? nullptr : &found->second;
}

const PixmapRecord *
ServerState::pixmap(std::uint32_t id) const
{
    const auto found = pixmaps_.find(id);
    return found == pixmaps_.end() ? nullptr : &found->second;
}

bool
ServerState::add_pixmap(PixmapRecord added, std::uint32_t owner)
{
    if (resource_exists(added.id) ||
        surface_bytes_ >
            maximum_server_surface_bytes - added.surface.storage_bytes()) {
        return false;
    }
    const std::uint32_t id = added.id;
    const std::size_t bytes = added.surface.storage_bytes();
    if (!resources_.insert(id, ResourceKind::pixmap, owner))
        return false;
    if (!pixmaps_.emplace(id, std::move(added)).second) {
        static_cast<void>(resources_.erase(id));
        return false;
    }
    surface_bytes_ += bytes;
    return true;
}

bool
ServerState::erase_pixmap(std::uint32_t id)
{
    const auto found = pixmaps_.find(id);
    if (found == pixmaps_.end())
        return false;
    surface_bytes_ -= found->second.surface.storage_bytes();
    pixmaps_.erase(found);
    static_cast<void>(resources_.erase(id));
    return true;
}

GraphicsContextRecord *
ServerState::graphics_context(std::uint32_t id)
{
    const auto found = graphics_contexts_.find(id);
    return found == graphics_contexts_.end() ? nullptr : &found->second;
}

const GraphicsContextRecord *
ServerState::graphics_context(std::uint32_t id) const
{
    const auto found = graphics_contexts_.find(id);
    return found == graphics_contexts_.end() ? nullptr : &found->second;
}

bool
ServerState::add_graphics_context(GraphicsContextRecord added,
                                  std::uint32_t owner)
{
    if (resource_exists(added.id) ||
        !resources_.insert(
            added.id, ResourceKind::graphics_context, owner)) {
        return false;
    }
    const std::uint32_t id = added.id;
    if (!graphics_contexts_.emplace(id, std::move(added)).second) {
        static_cast<void>(resources_.erase(id));
        return false;
    }
    return true;
}

bool
ServerState::erase_graphics_context(std::uint32_t id)
{
    if (graphics_contexts_.erase(id) == 0)
        return false;
    static_cast<void>(resources_.erase(id));
    return true;
}

Surface *
ServerState::drawable_surface(std::uint32_t id)
{
    if (auto *candidate = window(id))
        return candidate->surface ? &*candidate->surface : nullptr;
    if (auto *candidate = pixmap(id))
        return &candidate->surface;
    return nullptr;
}

const Surface *
ServerState::drawable_surface(std::uint32_t id) const
{
    if (const auto *candidate = window(id))
        return candidate->surface ? &*candidate->surface : nullptr;
    if (const auto *candidate = pixmap(id))
        return &candidate->surface;
    return nullptr;
}

std::uint8_t
ServerState::drawable_depth(std::uint32_t id) const
{
    const auto *surface = drawable_surface(id);
    return surface == nullptr ? 0 : surface->depth();
}

bool
ServerState::set_property(WindowRecord &candidate, AtomId property,
                          PropertyValue value)
{
    if (value.data.size() > maximum_property_bytes)
        return false;
    const auto found = candidate.properties.find(property);
    const std::size_t old_size = found == candidate.properties.end()
        ? 0
        : found->second.data.size();
    if (property_bytes_ - old_size >
        maximum_server_property_bytes - value.data.size()) {
        return false;
    }
    property_bytes_ = property_bytes_ - old_size + value.data.size();
    candidate.properties.insert_or_assign(property, std::move(value));
    return true;
}

void
ServerState::delete_property(WindowRecord &candidate, AtomId property)
{
    const auto found = candidate.properties.find(property);
    if (found == candidate.properties.end())
        return;
    property_bytes_ -= found->second.data.size();
    candidate.properties.erase(found);
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
    for (const auto &property : found->second.properties)
        property_bytes_ -= property.second.data.size();
    if (found->second.surface)
        surface_bytes_ -= found->second.surface->storage_bytes();
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
    const auto contexts =
        resources_.owned_by(owner, ResourceKind::graphics_context);
    for (const auto id : contexts)
        static_cast<void>(erase_graphics_context(id));
    const auto pixmaps = resources_.owned_by(owner, ResourceKind::pixmap);
    for (const auto id : pixmaps)
        static_cast<void>(erase_pixmap(id));
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

std::pair<std::int32_t, std::int32_t>
ServerState::absolute_position(std::uint32_t id) const
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    const auto *candidate = window(id);
    while (candidate != nullptr && candidate->parent != 0) {
        x += candidate->x + candidate->border_width;
        y += candidate->y + candidate->border_width;
        candidate = window(candidate->parent);
    }
    return {x, y};
}

} // namespace xmin::next
