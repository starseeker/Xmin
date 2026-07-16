#include "xmin/next/server_state.hpp"

#include "xmin/next/checked.hpp"

#include <algorithm>
#include <array>
#include <new>
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
    composited_root_ = Surface::create(width, height, 24);
    if (root.surface && composited_root_) {
        surface_bytes_ = root.surface->storage_bytes() +
            composited_root_->storage_bytes();
    }
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
    return root != nullptr && root->surface.has_value() &&
        composited_root_.has_value();
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
    added.owner = owner;
    if (!resources_.insert(id, ResourceKind::window, owner))
        return false;
    auto inserted = windows_.emplace(id, std::move(added));
    if (!inserted.second) {
        static_cast<void>(resources_.erase(id));
        return false;
    }
    window(parent_id)->children.push_back(id);
    surface_bytes_ += added_bytes;
    invalidate_scene();
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
    invalidate_scene();
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

Surface *
ServerState::readable_surface(std::uint32_t id)
{
    if (id != root_window_id)
        return drawable_surface(id);
    composite_scene();
    return composited_root_ ? &*composited_root_ : nullptr;
}

void
ServerState::set_window_mapped(WindowRecord &candidate, bool mapped) noexcept
{
    if (candidate.id == root_window_id)
        mapped = true;
    if (candidate.mapped == mapped)
        return;
    candidate.mapped = mapped;
    invalidate_scene();
}

void
ServerState::composite_scene()
{
    if (!scene_dirty_ || !composited_root_)
        return;
    const auto *root = window(root_window_id);
    if (root == nullptr || !root->surface)
        return;
    composited_root_->copy_from(*root->surface, 0, 0, 0, 0, width_, height_,
                                3, 0xffffffffU);
    for (const auto child : root->children) {
        composite_window(child, 0, 0, 0, 0, width_, height_);
    }
    scene_dirty_ = false;
}

void
ServerState::composite_window(std::uint32_t id, std::int64_t parent_x,
                              std::int64_t parent_y,
                              std::int64_t clip_left,
                              std::int64_t clip_top,
                              std::int64_t clip_right,
                              std::int64_t clip_bottom)
{
    const auto *candidate = window(id);
    if (candidate == nullptr || !candidate->mapped || !composited_root_)
        return;

    const std::int64_t outer_left = parent_x + candidate->x;
    const std::int64_t outer_top = parent_y + candidate->y;
    const std::int64_t content_left = outer_left + candidate->border_width;
    const std::int64_t content_top = outer_top + candidate->border_width;
    const std::int64_t content_right = content_left + candidate->width;
    const std::int64_t content_bottom = content_top + candidate->height;
    const std::int64_t outer_right =
        content_right + candidate->border_width;
    const std::int64_t outer_bottom =
        content_bottom + candidate->border_width;

    const auto intersect = [](std::int64_t first_left,
                              std::int64_t first_top,
                              std::int64_t first_right,
                              std::int64_t first_bottom,
                              std::int64_t second_left,
                              std::int64_t second_top,
                              std::int64_t second_right,
                              std::int64_t second_bottom) {
        return std::array<std::int64_t, 4>{
            std::max(first_left, second_left),
            std::max(first_top, second_top),
            std::min(first_right, second_right),
            std::min(first_bottom, second_bottom)};
    };
    const auto visible_outer = intersect(
        outer_left, outer_top, outer_right, outer_bottom,
        clip_left, clip_top, clip_right, clip_bottom);
    const auto visible_content = intersect(
        content_left, content_top, content_right, content_bottom,
        clip_left, clip_top, clip_right, clip_bottom);

    if (candidate->surface && visible_outer[0] < visible_outer[2] &&
        visible_outer[1] < visible_outer[3]) {
        composited_root_->fill(
            Rectangle{
                static_cast<std::int32_t>(visible_outer[0]),
                static_cast<std::int32_t>(visible_outer[1]),
                static_cast<std::uint32_t>(
                    visible_outer[2] - visible_outer[0]),
                static_cast<std::uint32_t>(
                    visible_outer[3] - visible_outer[1])},
            candidate->border_pixel, 3, 0xffffffffU);
    }
    if (candidate->surface && visible_content[0] < visible_content[2] &&
        visible_content[1] < visible_content[3]) {
        composited_root_->copy_from(
            *candidate->surface,
            static_cast<std::int32_t>(visible_content[0] - content_left),
            static_cast<std::int32_t>(visible_content[1] - content_top),
            static_cast<std::int32_t>(visible_content[0]),
            static_cast<std::int32_t>(visible_content[1]),
            static_cast<std::uint32_t>(
                visible_content[2] - visible_content[0]),
            static_cast<std::uint32_t>(
                visible_content[3] - visible_content[1]),
            3, 0xffffffffU);
    }

    if (visible_content[0] >= visible_content[2] ||
        visible_content[1] >= visible_content[3]) {
        return;
    }
    for (const auto child : candidate->children) {
        composite_window(child, content_left, content_top,
                         visible_content[0], visible_content[1],
                         visible_content[2], visible_content[3]);
    }
}

void
ServerState::advance_time() noexcept
{
    ++current_time_;
    if (current_time_ == 0)
        current_time_ = 1;
}

std::uint32_t
ServerState::selection_owner(AtomId selection) const
{
    const auto found = selections_.find(selection);
    return found == selections_.end() ? 0 : found->second.window;
}

bool
ServerState::can_queue_event(std::uint32_t client) const
{
    if (client == 0 || pending_events_ >= maximum_pending_events)
        return false;
    const auto found = event_queues_.find(client);
    return found == event_queues_.end() ||
        found->second.size() < maximum_pending_events_per_client;
}

bool
ServerState::queue_event(std::uint32_t client, ClientEvent event)
{
    if (!can_queue_event(client))
        return false;
    try {
        event_queues_[client].push_back(std::move(event));
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    ++pending_events_;
    return true;
}

SelectionUpdate
ServerState::set_selection_owner(AtomId selection, std::uint32_t window_id,
                                 std::uint32_t client, std::uint32_t time)
{
    const std::uint32_t effective_time = time == 0 ? current_time_ : time;
    const auto later_than = [](std::uint32_t left, std::uint32_t right) {
        return static_cast<std::int32_t>(left - right) > 0;
    };
    const auto earlier_than = [](std::uint32_t left, std::uint32_t right) {
        return static_cast<std::int32_t>(left - right) < 0;
    };
    if (later_than(effective_time, current_time_))
        return SelectionUpdate::ignored;

    auto found = selections_.find(selection);
    if (found != selections_.end() &&
        earlier_than(effective_time, found->second.changed_at)) {
        return SelectionUpdate::ignored;
    }

    const bool clear_previous = found != selections_.end() &&
        found->second.window != 0 &&
        (window_id == 0 || found->second.client != client);
    if (clear_previous && !can_queue_event(found->second.client))
        return SelectionUpdate::event_queue_full;

    if (found == selections_.end()) {
        try {
            found = selections_.emplace(selection, SelectionRecord{}).first;
        }
        catch (const std::bad_alloc &) {
            return SelectionUpdate::event_queue_full;
        }
    }
    const SelectionRecord previous = found->second;
    found->second = SelectionRecord{
        window_id, window_id == 0 ? 0 : client, effective_time};
    if (clear_previous) {
        const SelectionClearEvent event{
            effective_time, previous.window, selection};
        if (!queue_event(previous.client, event)) {
            found->second = previous;
            return SelectionUpdate::event_queue_full;
        }
    }
    return SelectionUpdate::updated;
}

EventDelivery
ServerState::deliver_client_message(std::uint32_t destination,
                                    std::uint32_t event_mask,
                                    bool propagate,
                                    const ClientMessageEvent &event)
{
    auto *candidate = window(destination);
    while (candidate != nullptr) {
        std::vector<std::uint32_t> recipients;
        try {
            if (event_mask == 0) {
                if (candidate->owner != 0)
                    recipients.push_back(candidate->owner);
            }
            else {
                for (const auto &selection : candidate->event_masks) {
                    if ((selection.second & event_mask) != 0)
                        recipients.push_back(selection.first);
                }
            }
        }
        catch (const std::bad_alloc &) {
            return EventDelivery::queue_full;
        }

        if (!recipients.empty()) {
            if (pending_events_ > maximum_pending_events - recipients.size())
                return EventDelivery::queue_full;
            for (const auto recipient : recipients) {
                if (!can_queue_event(recipient))
                    return EventDelivery::queue_full;
            }
            for (const auto recipient : recipients) {
                if (!queue_event(recipient, event))
                    return EventDelivery::queue_full;
            }
            return EventDelivery::delivered;
        }

        if (!propagate || candidate->parent == 0 || event_mask == 0)
            break;
        event_mask &= ~candidate->do_not_propagate_mask;
        if (event_mask == 0)
            break;
        candidate = window(candidate->parent);
    }
    return EventDelivery::no_recipient;
}

bool
ServerState::has_pending_event(std::uint32_t client) const
{
    const auto found = event_queues_.find(client);
    return found != event_queues_.end() && !found->second.empty();
}

const ClientEvent *
ServerState::next_event(std::uint32_t client) const
{
    const auto found = event_queues_.find(client);
    return found == event_queues_.end() || found->second.empty()
        ? nullptr
        : &found->second.front();
}

void
ServerState::pop_event(std::uint32_t client)
{
    const auto found = event_queues_.find(client);
    if (found == event_queues_.end() || found->second.empty())
        return;
    found->second.pop_front();
    --pending_events_;
    if (found->second.empty())
        event_queues_.erase(found);
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
    clear_selections_for_window(id);
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
    invalidate_scene();
}

void
ServerState::clear_selections_for_window(std::uint32_t window_id)
{
    for (auto &selection : selections_) {
        if (selection.second.window == window_id) {
            selection.second.window = 0;
            selection.second.client = 0;
            selection.second.changed_at = current_time_;
        }
    }
}

void
ServerState::disconnect_client(std::uint32_t owner)
{
    for (auto &window_entry : windows_)
        window_entry.second.event_masks.erase(owner);
    for (auto &selection : selections_) {
        if (selection.second.client == owner) {
            selection.second.window = 0;
            selection.second.client = 0;
            selection.second.changed_at = current_time_;
        }
    }
    const auto queued = event_queues_.find(owner);
    if (queued != event_queues_.end()) {
        pending_events_ -= queued->second.size();
        event_queues_.erase(queued);
    }
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
