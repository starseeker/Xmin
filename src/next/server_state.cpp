#include "xmin/next/server_state.hpp"

#include "xmin/next/checked.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace xmin::next {
namespace {

bool
overlaps(const PassiveGrab &grab, const PassiveGrabDomain &details,
         const PassiveGrabDomain &modifiers) noexcept
{
    return (grab.details & details).any() &&
        (grab.modifiers & modifiers).any();
}

bool
append_without(std::vector<PassiveGrab> &destination,
               const PassiveGrab &grab,
               const PassiveGrabDomain &removed_details,
               const PassiveGrabDomain &removed_modifiers)
{
    const PassiveGrabDomain retained_details =
        grab.details & ~removed_details;
    if (retained_details.any()) {
        if (destination.size() == maximum_passive_grabs)
            return false;
        PassiveGrab retained = grab;
        retained.details = retained_details;
        destination.push_back(std::move(retained));
    }

    const PassiveGrabDomain intersecting_details =
        grab.details & removed_details;
    const PassiveGrabDomain retained_modifiers =
        grab.modifiers & ~removed_modifiers;
    if (intersecting_details.any() && retained_modifiers.any()) {
        if (destination.size() == maximum_passive_grabs)
            return false;
        PassiveGrab retained = grab;
        retained.details = intersecting_details;
        retained.modifiers = retained_modifiers;
        destination.push_back(std::move(retained));
    }
    return true;
}

std::int16_t
wire_coordinate(std::int32_t value) noexcept
{
    return static_cast<std::int16_t>(std::clamp<std::int32_t>(
        value, std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
}

std::uint16_t
wire_size(std::uint32_t value) noexcept
{
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(
        value, std::numeric_limits<std::uint16_t>::max()));
}

bool
sync_trigger_fired(const SyncTrigger &trigger, std::int64_t old_value,
                   std::int64_t new_value) noexcept
{
    switch (trigger.test_type) {
    case SyncTestType::positive_transition:
        return old_value < trigger.test_value &&
            new_value >= trigger.test_value;
    case SyncTestType::negative_transition:
        return old_value > trigger.test_value &&
            new_value <= trigger.test_value;
    case SyncTestType::positive_comparison:
        return new_value >= trigger.test_value;
    case SyncTestType::negative_comparison:
        return new_value <= trigger.test_value;
    }
    return false;
}

std::int64_t
signed_from_bits(std::uint64_t bits) noexcept
{
    constexpr auto signed_max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (bits <= signed_max)
        return static_cast<std::int64_t>(bits);
    return -1 - static_cast<std::int64_t>(
        std::numeric_limits<std::uint64_t>::max() - bits);
}

std::uint64_t
nonnegative_distance(std::int64_t greater, std::int64_t lesser) noexcept
{
    return static_cast<std::uint64_t>(greater) -
        static_cast<std::uint64_t>(lesser);
}

struct AdvancedAlarm {
    std::int64_t test_value;
    std::uint8_t state;
};

AdvancedAlarm
advance_alarm(const SyncAlarmRecord &alarm, std::int64_t counter_value) noexcept
{
    constexpr std::uint8_t active = 0;
    constexpr std::uint8_t inactive = 1;
    if (alarm.state != active)
        return {alarm.trigger.test_value, alarm.state};

    if (alarm.delta == 0 &&
        (alarm.trigger.test_type == SyncTestType::positive_comparison ||
         alarm.trigger.test_type == SyncTestType::negative_comparison)) {
        return {alarm.trigger.test_value, inactive};
    }

    if (alarm.trigger.test_type == SyncTestType::positive_transition ||
        alarm.trigger.test_type == SyncTestType::negative_transition) {
        const auto next = checked_add(alarm.trigger.test_value, alarm.delta);
        return next ? AdvancedAlarm{*next, active}
                    : AdvancedAlarm{alarm.trigger.test_value, inactive};
    }

    const bool positive =
        alarm.trigger.test_type == SyncTestType::positive_comparison;
    const std::uint64_t magnitude = positive
        ? static_cast<std::uint64_t>(alarm.delta)
        : std::uint64_t{0} - static_cast<std::uint64_t>(alarm.delta);
    if (magnitude == 0)
        return {alarm.trigger.test_value, inactive};

    const std::uint64_t distance = positive
        ? nonnegative_distance(counter_value, alarm.trigger.test_value)
        : nonnegative_distance(alarm.trigger.test_value, counter_value);
    const std::uint64_t quotient = distance / magnitude;
    if (quotient == std::numeric_limits<std::uint64_t>::max())
        return {alarm.trigger.test_value, inactive};
    const std::uint64_t steps = quotient + 1;
    if (steps > std::numeric_limits<std::uint64_t>::max() / magnitude)
        return {alarm.trigger.test_value, inactive};
    const std::uint64_t movement = steps * magnitude;
    const std::uint64_t available = positive
        ? nonnegative_distance(std::numeric_limits<std::int64_t>::max(),
                               alarm.trigger.test_value)
        : nonnegative_distance(alarm.trigger.test_value,
                               std::numeric_limits<std::int64_t>::min());
    if (movement > available)
        return {alarm.trigger.test_value, inactive};
    const std::uint64_t bits = positive
        ? static_cast<std::uint64_t>(alarm.trigger.test_value) + movement
        : static_cast<std::uint64_t>(alarm.trigger.test_value) - movement;
    return {signed_from_bits(bits), active};
}

bool
sync_notification_eligible(const SyncWaitCondition &condition,
                           std::int64_t value) noexcept
{
    const auto difference = checked_subtract(
        value, condition.trigger.test_value);
    if (!difference)
        return false;
    const bool positive =
        condition.trigger.test_type == SyncTestType::positive_transition ||
        condition.trigger.test_type == SyncTestType::positive_comparison;
    return positive ? *difference >= condition.event_threshold
                    : *difference <= condition.event_threshold;
}

} // namespace

PassiveGrabDomain
passive_grab_details(PassiveGrabKind kind, std::uint8_t detail) noexcept
{
    PassiveGrabDomain result;
    if (detail != 0) {
        result.set(detail);
        return result;
    }

    result.set();
    result.reset(0);
    if (kind == PassiveGrabKind::key) {
        for (std::size_t keycode = 1; keycode < minimum_keycode; ++keycode)
            result.reset(keycode);
    }
    return result;
}

PassiveGrabDomain
passive_grab_modifiers(std::uint16_t modifiers) noexcept
{
    PassiveGrabDomain result;
    if (modifiers == any_modifier)
        result.set();
    else if (modifiers <= all_modifiers_mask)
        result.set(modifiers);
    return result;
}

ServerState::ServerState(std::uint16_t width, std::uint16_t height,
                         Clock &clock)
    : width_(width), height_(height), clock_(clock)
{
    input_.pointer_x = width / 2;
    input_.pointer_y = height / 2;

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

PassiveGrabUpdate
ServerState::add_passive_grab(PassiveGrab added)
{
    if (added.details.none() || added.modifiers.none())
        return PassiveGrabUpdate::resource_exhausted;

    for (const auto &existing : passive_grabs_) {
        if (existing.kind == added.kind && existing.window == added.window &&
            existing.owner != added.owner &&
            overlaps(existing, added.details, added.modifiers)) {
            return PassiveGrabUpdate::access_denied;
        }
    }

    try {
        std::vector<PassiveGrab> revised;
        revised.reserve(std::min(maximum_passive_grabs,
                                 passive_grabs_.size() * 2 + 1));
        for (const auto &existing : passive_grabs_) {
            if (existing.kind == added.kind &&
                existing.window == added.window &&
                existing.owner == added.owner &&
                overlaps(existing, added.details, added.modifiers)) {
                if (!append_without(revised, existing, added.details,
                                    added.modifiers)) {
                    return PassiveGrabUpdate::resource_exhausted;
                }
            }
            else {
                if (revised.size() == maximum_passive_grabs)
                    return PassiveGrabUpdate::resource_exhausted;
                revised.push_back(existing);
            }
        }
        if (revised.size() == maximum_passive_grabs)
            return PassiveGrabUpdate::resource_exhausted;
        revised.push_back(std::move(added));
        const std::size_t owner_count = static_cast<std::size_t>(std::count_if(
            revised.begin(), revised.end(),
            [owner = revised.back().owner](const PassiveGrab &grab) {
                return grab.owner == owner;
            }));
        if (owner_count > maximum_passive_grabs_per_client)
            return PassiveGrabUpdate::resource_exhausted;
        passive_grabs_ = std::move(revised);
        return PassiveGrabUpdate::updated;
    }
    catch (const std::bad_alloc &) {
        return PassiveGrabUpdate::resource_exhausted;
    }
}

PassiveGrabUpdate
ServerState::remove_passive_grab(
    PassiveGrabKind kind, std::uint32_t owner, std::uint32_t window_id,
    const PassiveGrabDomain &details,
    const PassiveGrabDomain &modifiers)
{
    try {
        std::vector<PassiveGrab> revised;
        revised.reserve(std::min(maximum_passive_grabs,
                                 passive_grabs_.size() * 2));
        for (const auto &existing : passive_grabs_) {
            if (existing.kind == kind && existing.window == window_id &&
                existing.owner == owner &&
                overlaps(existing, details, modifiers)) {
                if (!append_without(revised, existing, details, modifiers))
                    return PassiveGrabUpdate::resource_exhausted;
            }
            else {
                if (revised.size() == maximum_passive_grabs)
                    return PassiveGrabUpdate::resource_exhausted;
                revised.push_back(existing);
            }
        }
        const std::size_t owner_count = static_cast<std::size_t>(std::count_if(
            revised.begin(), revised.end(),
            [owner](const PassiveGrab &grab) { return grab.owner == owner; }));
        if (owner_count > maximum_passive_grabs_per_client)
            return PassiveGrabUpdate::resource_exhausted;
        passive_grabs_ = std::move(revised);
        return PassiveGrabUpdate::updated;
    }
    catch (const std::bad_alloc &) {
        return PassiveGrabUpdate::resource_exhausted;
    }
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

std::optional<std::uint32_t>
ServerState::resource_owner(std::uint32_t id) const
{
    const auto resource = resources_.find(id);
    if (!resource)
        return std::nullopt;
    return resource->owner;
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

bool
ServerState::colormap_exists(std::uint32_t id) const
{
    return resources_.is(id, ResourceKind::colormap);
}

bool
ServerState::add_colormap(std::uint32_t id, std::uint32_t owner)
{
    return !resource_exists(id) &&
        resources_.insert(id, ResourceKind::colormap, owner);
}

bool
ServerState::erase_colormap(std::uint32_t id)
{
    if (id == default_colormap_id || !colormap_exists(id))
        return false;
    if (installed_colormap_ == id)
        installed_colormap_ = default_colormap_id;
    for (auto &entry : windows_) {
        if (entry.second.colormap == id)
            entry.second.colormap = default_colormap_id;
    }
    return resources_.erase(id);
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

EventDelivery
ServerState::set_window_mapped(WindowRecord &candidate, bool mapped)
{
    if (candidate.id == root_window_id)
        mapped = true;
    if (candidate.mapped == mapped)
        return EventDelivery::no_recipient;
    const std::array<std::uint32_t, 1> changed{candidate.id};
    return update_window_mappings(changed.data(), changed.size(), mapped);
}

EventDelivery
ServerState::update_window_mappings(
    const std::uint32_t *changed, std::size_t count, bool mapped)
{
    const std::uint32_t old_pointer_window = deepest_window_at(
        root_window_id, input_.pointer_x, input_.pointer_y);
    const FocusState old_focus = input_.focus;
    for (std::size_t index = 0; index < count; ++index) {
        auto *candidate = window(changed[index]);
        if (candidate != nullptr && candidate->id != root_window_id)
            candidate->mapped = mapped;
    }
    const std::uint32_t new_pointer_window = deepest_window_at(
        root_window_id, input_.pointer_x, input_.pointer_y);
    FocusState new_focus = old_focus;
    if (!mapped && old_focus.kind == FocusKind::window &&
        map_state(old_focus.window) != 2) {
        new_focus = reverted_focus_state();
    }
    const bool pointer_grab_lost = !mapped && input_.pointer_grab &&
        (map_state(input_.pointer_grab->window) != 2 ||
         (input_.pointer_grab->confine_to != 0 &&
          map_state(input_.pointer_grab->confine_to) != 2));
    const bool keyboard_grab_lost = !mapped && input_.keyboard_grab &&
        map_state(input_.keyboard_grab->window) != 2;

    std::vector<PlannedEvent> events;
    EventDelivery pointer_ungrab = EventDelivery::no_recipient;
    if (pointer_grab_lost) {
        pointer_ungrab = append_crossing_events(
            input_.pointer_grab->window, old_pointer_window,
            input_.pointer_x, input_.pointer_y,
            input_.modifier_button_mask, 2, nullptr, old_focus, events);
    }
    EventDelivery keyboard_ungrab = EventDelivery::no_recipient;
    if (keyboard_grab_lost &&
        pointer_ungrab != EventDelivery::queue_full) {
        FocusState grabbed_focus = old_focus;
        grabbed_focus.kind = FocusKind::window;
        grabbed_focus.window = input_.keyboard_grab->window;
        keyboard_ungrab = append_focus_events(
            grabbed_focus, old_focus, 2, old_pointer_window, events);
    }
    EventDelivery focus = EventDelivery::no_recipient;
    if (pointer_ungrab != EventDelivery::queue_full &&
        keyboard_ungrab != EventDelivery::queue_full &&
        (old_focus.kind != new_focus.kind ||
         old_focus.window != new_focus.window)) {
        focus = append_focus_events(
            old_focus, new_focus,
            input_.keyboard_grab && !keyboard_grab_lost ? 3 : 0,
            old_pointer_window, events);
    }
    EventDelivery crossing = EventDelivery::no_recipient;
    if (pointer_ungrab != EventDelivery::queue_full &&
        keyboard_ungrab != EventDelivery::queue_full &&
        focus != EventDelivery::queue_full &&
        old_pointer_window != new_pointer_window) {
        crossing = append_crossing_events(
            old_pointer_window, new_pointer_window,
            input_.pointer_x, input_.pointer_y,
            input_.modifier_button_mask, 0,
            pointer_grab_lost ? nullptr :
                (input_.pointer_grab ? &*input_.pointer_grab : nullptr),
            new_focus, events);
    }
    for (std::size_t index = 0; index < count; ++index) {
        auto *candidate = window(changed[index]);
        if (candidate != nullptr && candidate->id != root_window_id)
            candidate->mapped = !mapped;
    }
    if (pointer_ungrab == EventDelivery::queue_full ||
        keyboard_ungrab == EventDelivery::queue_full ||
        crossing == EventDelivery::queue_full ||
        focus == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return EventDelivery::queue_full;
    }

    for (std::size_t index = 0; index < count; ++index) {
        auto *candidate = window(changed[index]);
        if (candidate != nullptr && candidate->id != root_window_id)
            candidate->mapped = mapped;
    }
    input_.focus = new_focus;
    if (pointer_grab_lost)
        input_.pointer_grab.reset();
    if (keyboard_grab_lost)
        input_.keyboard_grab.reset();
    invalidate_scene();
    return pointer_ungrab == EventDelivery::delivered ||
            keyboard_ungrab == EventDelivery::delivered ||
            crossing == EventDelivery::delivered ||
            focus == EventDelivery::delivered
        ? EventDelivery::delivered
        : EventDelivery::no_recipient;
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

    const auto for_each_shape_rectangle = [candidate](
                                              std::uint8_t kind,
                                              const auto &operation) {
        if (candidate->shapes[kind]) {
            for (const auto &rectangle :
                 candidate->shapes[kind]->rectangles()) {
                operation(rectangle);
            }
            return;
        }
        operation(candidate->default_shape(kind));
    };
    if (candidate->surface && visible_outer[0] < visible_outer[2] &&
        visible_outer[1] < visible_outer[3] &&
        candidate->border_width != 0) {
        for_each_shape_rectangle(0, [&](const Rectangle &shape) {
            const auto bounded = intersect(
                content_left + shape.x, content_top + shape.y,
                content_left + shape.x + shape.width,
                content_top + shape.y + shape.height,
                visible_outer[0], visible_outer[1],
                visible_outer[2], visible_outer[3]);
            const std::array<std::array<std::int64_t, 4>, 4> bands{{
                {{outer_left, outer_top, outer_right, content_top}},
                {{outer_left, content_bottom, outer_right, outer_bottom}},
                {{outer_left, content_top, content_left, content_bottom}},
                {{content_right, content_top, outer_right, content_bottom}},
            }};
            for (const auto &band : bands) {
                const auto shaped = intersect(
                    bounded[0], bounded[1], bounded[2], bounded[3],
                    band[0], band[1], band[2], band[3]);
                if (shaped[0] >= shaped[2] || shaped[1] >= shaped[3])
                    continue;
                composited_root_->fill(
                    Rectangle{
                        static_cast<std::int32_t>(shaped[0]),
                        static_cast<std::int32_t>(shaped[1]),
                        static_cast<std::uint32_t>(shaped[2] - shaped[0]),
                        static_cast<std::uint32_t>(shaped[3] - shaped[1])},
                    candidate->border_pixel, 3, 0xffffffffU);
            }
        });
    }
    if (candidate->surface && visible_content[0] < visible_content[2] &&
        visible_content[1] < visible_content[3]) {
        Region combined_shape;
        const Region *content_shape = nullptr;
        if (candidate->shapes[0] && candidate->shapes[1]) {
            if (!Region::combine(
                    RegionOperation::intersect, *candidate->shapes[0],
                    *candidate->shapes[1], combined_shape)) {
                return;
            }
            content_shape = &combined_shape;
        }
        else if (candidate->shapes[0]) {
            content_shape = &*candidate->shapes[0];
        }
        else if (candidate->shapes[1]) {
            content_shape = &*candidate->shapes[1];
        }
        const auto copy_rectangle = [&](const Rectangle &shape) {
            const auto shaped = intersect(
                content_left + shape.x, content_top + shape.y,
                content_left + shape.x + shape.width,
                content_top + shape.y + shape.height,
                visible_content[0], visible_content[1],
                visible_content[2], visible_content[3]);
            if (shaped[0] >= shaped[2] || shaped[1] >= shaped[3])
                return;
            composited_root_->copy_from(
                *candidate->surface,
                static_cast<std::int32_t>(shaped[0] - content_left),
                static_cast<std::int32_t>(shaped[1] - content_top),
                static_cast<std::int32_t>(shaped[0]),
                static_cast<std::int32_t>(shaped[1]),
                static_cast<std::uint32_t>(shaped[2] - shaped[0]),
                static_cast<std::uint32_t>(shaped[3] - shaped[1]),
                3, 0xffffffffU);
        };
        if (content_shape) {
            for (const auto &shape : content_shape->rectangles())
                copy_rectangle(shape);
        }
        else {
            copy_rectangle(candidate->default_shape(1));
        }
    }

    if (visible_content[0] >= visible_content[2] ||
        visible_content[1] >= visible_content[3]) {
        return;
    }
    auto child_clip = visible_content;
    if (candidate->shapes[0]) {
        const Rectangle bounding = candidate->shapes[0]->extents();
        child_clip = intersect(
            child_clip[0], child_clip[1], child_clip[2], child_clip[3],
            content_left + bounding.x, content_top + bounding.y,
            content_left + bounding.x + bounding.width,
            content_top + bounding.y + bounding.height);
    }
    for (const auto child : candidate->children) {
        composite_window(child, content_left, content_top,
                         child_clip[0], child_clip[1],
                         child_clip[2], child_clip[3]);
    }
}

void
ServerState::advance_time() noexcept
{
    ++current_time_;
    if (current_time_ == 0)
        current_time_ = 1;
}

FocusUpdate
ServerState::set_input_focus(FocusKind kind, std::uint32_t window_id,
                             std::uint8_t revert_to,
                             std::uint32_t time)
{
    const std::uint32_t effective_time = time == 0 ? current_time_ : time;
    const auto later_than = [](std::uint32_t left, std::uint32_t right) {
        return static_cast<std::int32_t>(left - right) > 0;
    };
    const auto earlier_than = [](std::uint32_t left, std::uint32_t right) {
        return static_cast<std::int32_t>(left - right) < 0;
    };
    if (later_than(effective_time, current_time_) ||
        earlier_than(effective_time, input_.focus.changed_at)) {
        return FocusUpdate::ignored;
    }
    FocusState next = input_.focus;
    next.kind = kind;
    next.window = kind == FocusKind::window ? window_id : 0;
    next.changed_at = effective_time;
    next.revert_to = revert_to;
    std::vector<PlannedEvent> events;
    const std::uint32_t pointer_window = deepest_window_at(
        root_window_id, input_.pointer_x, input_.pointer_y);
    if (append_focus_events(input_.focus, next, 0, pointer_window, events) ==
            EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return FocusUpdate::queue_full;
    }
    input_.focus = next;
    return FocusUpdate::updated;
}

EventDelivery
ServerState::activate_pointer_grab(ActiveGrab grab)
{
    const std::uint32_t pointer_window = deepest_window_at(
        root_window_id, input_.pointer_x, input_.pointer_y);
    const std::uint32_t old_window = input_.pointer_grab
        ? input_.pointer_grab->window
        : pointer_window;
    std::vector<PlannedEvent> events;
    const EventDelivery crossing = append_crossing_events(
        old_window, grab.window, input_.pointer_x, input_.pointer_y,
        input_.modifier_button_mask, 1, nullptr, input_.focus, events);
    if (crossing == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return EventDelivery::queue_full;
    }
    input_.pointer_grab_time = grab.activated_at;
    input_.pointer_grab = std::move(grab);
    return crossing;
}

EventDelivery
ServerState::deactivate_pointer_grab()
{
    if (!input_.pointer_grab)
        return EventDelivery::no_recipient;
    const std::uint32_t pointer_window = deepest_window_at(
        root_window_id, input_.pointer_x, input_.pointer_y);
    std::vector<PlannedEvent> events;
    const EventDelivery crossing = append_crossing_events(
        input_.pointer_grab->window, pointer_window,
        input_.pointer_x, input_.pointer_y,
        input_.modifier_button_mask, 2, nullptr, input_.focus, events);
    if (crossing == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return EventDelivery::queue_full;
    }
    input_.pointer_grab.reset();
    return crossing;
}

EventDelivery
ServerState::activate_keyboard_grab(ActiveGrab grab)
{
    FocusState old_focus = input_.focus;
    if (input_.keyboard_grab) {
        old_focus.kind = FocusKind::window;
        old_focus.window = input_.keyboard_grab->window;
    }
    FocusState grabbed_focus = input_.focus;
    grabbed_focus.kind = FocusKind::window;
    grabbed_focus.window = grab.window;
    std::vector<PlannedEvent> events;
    EventDelivery focus = EventDelivery::no_recipient;
    if (!input_.keyboard_grab || old_focus.window != grab.window) {
        const std::uint32_t pointer_window = deepest_window_at(
            root_window_id, input_.pointer_x, input_.pointer_y);
        focus = append_focus_events(
            old_focus, grabbed_focus, 1, pointer_window, events);
    }
    if (focus == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return EventDelivery::queue_full;
    }
    input_.keyboard_grab_time = grab.activated_at;
    input_.keyboard_grab = std::move(grab);
    return focus;
}

EventDelivery
ServerState::deactivate_keyboard_grab()
{
    if (!input_.keyboard_grab)
        return EventDelivery::no_recipient;
    FocusState grabbed_focus = input_.focus;
    grabbed_focus.kind = FocusKind::window;
    grabbed_focus.window = input_.keyboard_grab->window;
    const std::uint32_t pointer_window = deepest_window_at(
        root_window_id, input_.pointer_x, input_.pointer_y);
    std::vector<PlannedEvent> events;
    const EventDelivery focus = append_focus_events(
        grabbed_focus, input_.focus, 2, pointer_window, events);
    if (focus == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return EventDelivery::queue_full;
    }
    input_.keyboard_grab.reset();
    return focus;
}

std::uint32_t
ServerState::selection_owner(AtomId selection) const
{
    const auto found = selections_.find(selection);
    return found == selections_.end() ? 0 : found->second.window;
}

bool
ServerState::register_client(std::uint32_t client)
{
    if (client == 0)
        return false;
    const auto found = std::find_if(
        clients_.begin(), clients_.end(),
        [client](const auto &entry) { return entry.first == client; });
    if (found != clients_.end())
        return true;
    try {
        clients_.emplace_back(client, 0);
        sync_priorities_.emplace(client, 0);
    }
    catch (const std::bad_alloc &) {
        clients_.erase(
            std::remove_if(
                clients_.begin(), clients_.end(),
                [client](const auto &entry) { return entry.first == client; }),
            clients_.end());
        sync_priorities_.erase(client);
        return false;
    }
    return true;
}

void
ServerState::note_client_sequence(std::uint32_t client,
                                  std::uint16_t sequence) noexcept
{
    const auto found = std::find_if(
        clients_.begin(), clients_.end(),
        [client](const auto &entry) { return entry.first == client; });
    if (found != clients_.end())
        found->second = sequence;
}

std::uint16_t
ServerState::client_sequence(std::uint32_t client) const noexcept
{
    const auto found = std::find_if(
        clients_.begin(), clients_.end(),
        [client](const auto &entry) { return entry.first == client; });
    return found == clients_.end() ? 0 : found->second;
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
    const std::uint16_t sequence = client_sequence(client);
    std::visit(
        [sequence](auto &value) { value.sequence = sequence; }, event);
    try {
        event_queues_[client].push_back(std::move(event));
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    ++pending_events_;
    return true;
}

bool
ServerState::queue_events_atomically(const std::vector<PlannedEvent> &events)
{
    if (events.size() > maximum_pending_events - pending_events_)
        return false;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const std::uint32_t recipient = events[index].first;
        if (recipient == 0)
            return false;
        std::size_t additions = 1;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (events[previous].first == recipient)
                ++additions;
        }
        const auto found = event_queues_.find(recipient);
        const std::size_t queued = found == event_queues_.end()
            ? 0
            : found->second.size();
        if (additions > maximum_pending_events_per_client - queued)
            return false;
    }

    for (std::size_t index = 0; index < events.size(); ++index) {
        if (queue_event(events[index].first, events[index].second))
            continue;
        const auto failed = event_queues_.find(events[index].first);
        if (failed != event_queues_.end() && failed->second.empty())
            event_queues_.erase(failed);
        while (index > 0) {
            --index;
            const auto found = event_queues_.find(events[index].first);
            if (found == event_queues_.end() || found->second.empty())
                continue;
            found->second.pop_back();
            --pending_events_;
            if (found->second.empty())
                event_queues_.erase(found);
        }
        return false;
    }
    return true;
}

bool
ServerState::broadcast_mapping_notify(std::uint8_t request,
                                      std::uint8_t first_keycode,
                                      std::uint8_t count)
{
    if (clients_.size() > maximum_pending_events - pending_events_)
        return false;
    for (const auto &client : clients_) {
        if (!can_queue_event(client.first))
            return false;
    }

    std::vector<std::uint32_t> queued;
    try {
        queued.reserve(clients_.size());
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    const MappingNotifyEvent event{request, first_keycode, count};
    for (const auto &client : clients_) {
        if (queue_event(client.first, event)) {
            queued.push_back(client.first);
            continue;
        }

        const auto failed = event_queues_.find(client.first);
        if (failed != event_queues_.end() && failed->second.empty())
            event_queues_.erase(failed);
        for (const auto recipient : queued) {
            const auto found = event_queues_.find(recipient);
            if (found == event_queues_.end() || found->second.empty())
                continue;
            found->second.pop_back();
            --pending_events_;
            if (found->second.empty())
                event_queues_.erase(found);
        }
        return false;
    }
    return true;
}

ShapeUpdate
ServerState::set_window_shape(WindowRecord &candidate, std::uint8_t kind,
                              std::optional<Region> shape)
{
    if (kind >= candidate.shapes.size())
        return ShapeUpdate::invalid;
    if (shape && shape->rectangles().size() > maximum_shape_rectangles)
        return ShapeUpdate::resource_exhausted;
    if (candidate.parent == 0)
        return ShapeUpdate::updated;
    const Rectangle extents = shape ? shape->extents()
                                    : candidate.default_shape(kind);
    std::vector<PlannedEvent> events;
    try {
        events.reserve(candidate.shape_event_clients.size());
        for (const auto client : candidate.shape_event_clients) {
            events.emplace_back(
                client,
                ShapeNotifyEvent{
                    kind, candidate.id, wire_coordinate(extents.x),
                    wire_coordinate(extents.y), wire_size(extents.width),
                    wire_size(extents.height), current_time_,
                    shape.has_value()});
        }
    }
    catch (const std::bad_alloc &) {
        return ShapeUpdate::queue_full;
    }
    if (!queue_events_atomically(events))
        return ShapeUpdate::queue_full;
    candidate.shapes[kind] = std::move(shape);
    invalidate_scene();
    return ShapeUpdate::updated;
}

bool
ServerState::select_shape_events(WindowRecord &candidate,
                                 std::uint32_t client, bool enabled)
{
    const auto found = std::find(
        candidate.shape_event_clients.begin(),
        candidate.shape_event_clients.end(), client);
    if (!enabled) {
        if (found != candidate.shape_event_clients.end())
            candidate.shape_event_clients.erase(found);
        return true;
    }
    if (found != candidate.shape_event_clients.end())
        return true;
    try {
        candidate.shape_event_clients.push_back(client);
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    return true;
}

bool
ServerState::shape_events_selected(const WindowRecord &candidate,
                                   std::uint32_t client) const noexcept
{
    return std::find(
               candidate.shape_event_clients.begin(),
               candidate.shape_event_clients.end(), client) !=
        candidate.shape_event_clients.end();
}

SyncCounterRecord *
ServerState::sync_counter(std::uint32_t id)
{
    const auto found = sync_counters_.find(id);
    return found == sync_counters_.end() ? nullptr : &found->second;
}

const SyncCounterRecord *
ServerState::sync_counter(std::uint32_t id) const
{
    const auto found = sync_counters_.find(id);
    return found == sync_counters_.end() ? nullptr : &found->second;
}

bool
ServerState::add_sync_counter(SyncCounterRecord counter, std::uint32_t owner)
{
    const std::uint32_t id = counter.id;
    if (!resources_.insert(id, ResourceKind::sync_counter, owner))
        return false;
    try {
        if (!sync_counters_.emplace(id, std::move(counter)).second) {
            static_cast<void>(resources_.erase(id));
            return false;
        }
    }
    catch (const std::bad_alloc &) {
        static_cast<void>(resources_.erase(id));
        return false;
    }
    return true;
}

bool
ServerState::append_sync_alarm_event(
    const SyncAlarmRecord &alarm, std::int64_t counter_value,
    std::int64_t alarm_value, std::uint8_t state,
    std::vector<PlannedEvent> &events) const
{
    try {
        for (const auto client : alarm.event_clients) {
            events.emplace_back(
                client, SyncAlarmNotifyEvent{
                    alarm.id, counter_value, alarm_value, current_time_,
                    state});
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    return true;
}

SyncUpdate
ServerState::update_sync_counter(SyncCounterRecord &counter,
                                 std::int64_t value, bool destroying)
{
    const std::int64_t old_value = counter.value;
    std::vector<SyncAlarmRecord> revised_alarms;
    std::vector<std::uint32_t> completed_waits;
    std::vector<PlannedEvent> events;
    try {
        revised_alarms.reserve(sync_alarms_.size());
        completed_waits.reserve(sync_counter_waits_.size());

        for (const auto &entry : sync_alarms_) {
            if (entry.second.trigger.counter != counter.id)
                continue;
            SyncAlarmRecord revised = entry.second;
            if (destroying) {
                if (!append_sync_alarm_event(
                        revised, old_value, revised.trigger.test_value, 1,
                        events)) {
                    return SyncUpdate::resource_exhausted;
                }
                revised.state = 1;
                revised.trigger.counter = 0;
                revised_alarms.push_back(std::move(revised));
                continue;
            }
            if (!sync_trigger_fired(revised.trigger, old_value, value))
                continue;
            const std::int64_t alarm_value = revised.trigger.test_value;
            const AdvancedAlarm advanced = advance_alarm(revised, value);
            if (!append_sync_alarm_event(
                    revised, value, alarm_value, advanced.state, events)) {
                return SyncUpdate::resource_exhausted;
            }
            revised.trigger.test_value = advanced.test_value;
            revised.state = advanced.state;
            revised_alarms.push_back(std::move(revised));
        }

        for (const auto &entry : sync_counter_waits_) {
            bool fired = false;
            for (const auto &condition : entry.second) {
                if (condition.trigger.counter != counter.id)
                    continue;
                if (destroying || sync_trigger_fired(
                        condition.trigger, old_value, value)) {
                    fired = true;
                    break;
                }
            }
            if (!fired)
                continue;

            std::vector<SyncCounterNotifyEvent> notifications;
            notifications.reserve(entry.second.size());
            for (const auto &condition : entry.second) {
                const bool destroyed = destroying &&
                    condition.trigger.counter == counter.id;
                std::int64_t current = 0;
                if (condition.trigger.counter == counter.id) {
                    current = destroying ? old_value : value;
                }
                else {
                    const auto *other = sync_counter(
                        condition.trigger.counter);
                    if (other == nullptr)
                        continue;
                    current = other->value;
                }
                if (!destroyed &&
                    !sync_notification_eligible(condition, current)) {
                    continue;
                }
                notifications.push_back(SyncCounterNotifyEvent{
                    condition.trigger.counter,
                    condition.trigger.test_value, current, current_time_, 0,
                    destroyed});
            }
            for (std::size_t index = 0; index < notifications.size();
                 ++index) {
                notifications[index].count = static_cast<std::uint16_t>(
                    notifications.size() - index - 1);
                events.emplace_back(entry.first, notifications[index]);
            }
            completed_waits.push_back(entry.first);
        }
    }
    catch (const std::bad_alloc &) {
        return SyncUpdate::resource_exhausted;
    }

    if (!queue_events_atomically(events))
        return SyncUpdate::queue_full;
    for (auto &alarm : revised_alarms) {
        const auto found = sync_alarms_.find(alarm.id);
        if (found != sync_alarms_.end())
            found->second = std::move(alarm);
    }
    for (const auto client : completed_waits)
        sync_counter_waits_.erase(client);
    if (!destroying)
        counter.value = value;
    return SyncUpdate::updated;
}

SyncUpdate
ServerState::set_sync_counter(SyncCounterRecord &counter, std::int64_t value)
{
    return update_sync_counter(counter, value, false);
}

SyncUpdate
ServerState::erase_sync_counter(std::uint32_t id)
{
    auto found = sync_counters_.find(id);
    if (found == sync_counters_.end())
        return SyncUpdate::invalid;
    const auto updated = update_sync_counter(found->second, 0, true);
    if (updated != SyncUpdate::updated)
        return updated;
    sync_counters_.erase(found);
    static_cast<void>(resources_.erase(id));
    return SyncUpdate::updated;
}

SyncAlarmRecord *
ServerState::sync_alarm(std::uint32_t id)
{
    const auto found = sync_alarms_.find(id);
    return found == sync_alarms_.end() ? nullptr : &found->second;
}

const SyncAlarmRecord *
ServerState::sync_alarm(std::uint32_t id) const
{
    const auto found = sync_alarms_.find(id);
    return found == sync_alarms_.end() ? nullptr : &found->second;
}

SyncUpdate
ServerState::commit_sync_alarm(SyncAlarmRecord alarm, bool creating)
{
    std::vector<PlannedEvent> events;
    const auto *counter = sync_counter(alarm.trigger.counter);
    alarm.state = counter == nullptr ? 1 : 0;
    if (counter != nullptr && sync_trigger_fired(
            alarm.trigger, counter->value, counter->value)) {
        const std::int64_t alarm_value = alarm.trigger.test_value;
        const AdvancedAlarm advanced = advance_alarm(alarm, counter->value);
        if (!append_sync_alarm_event(
                alarm, counter->value, alarm_value, advanced.state, events)) {
            return SyncUpdate::resource_exhausted;
        }
        alarm.trigger.test_value = advanced.test_value;
        alarm.state = advanced.state;
    }

    if (creating) {
        if (!resources_.insert(
                alarm.id, ResourceKind::sync_alarm, alarm.owner)) {
            return SyncUpdate::invalid;
        }
        try {
            if (!sync_alarms_.emplace(alarm.id, alarm).second) {
                static_cast<void>(resources_.erase(alarm.id));
                return SyncUpdate::invalid;
            }
        }
        catch (const std::bad_alloc &) {
            static_cast<void>(resources_.erase(alarm.id));
            return SyncUpdate::resource_exhausted;
        }
    }

    if (!queue_events_atomically(events)) {
        if (creating) {
            sync_alarms_.erase(alarm.id);
            static_cast<void>(resources_.erase(alarm.id));
        }
        return SyncUpdate::queue_full;
    }
    if (creating) {
        sync_alarms_.find(alarm.id)->second = std::move(alarm);
    }
    else {
        const auto found = sync_alarms_.find(alarm.id);
        if (found == sync_alarms_.end())
            return SyncUpdate::invalid;
        found->second = std::move(alarm);
    }
    return SyncUpdate::updated;
}

SyncUpdate
ServerState::add_sync_alarm(SyncAlarmRecord alarm, std::uint32_t owner)
{
    alarm.owner = owner;
    return commit_sync_alarm(std::move(alarm), true);
}

SyncUpdate
ServerState::change_sync_alarm(SyncAlarmRecord alarm)
{
    if (sync_alarm(alarm.id) == nullptr)
        return SyncUpdate::invalid;
    return commit_sync_alarm(std::move(alarm), false);
}

SyncUpdate
ServerState::erase_sync_alarm(std::uint32_t id)
{
    const auto found = sync_alarms_.find(id);
    if (found == sync_alarms_.end())
        return SyncUpdate::invalid;
    std::vector<PlannedEvent> events;
    if (const auto *counter = sync_counter(found->second.trigger.counter)) {
        if (!append_sync_alarm_event(
                found->second, counter->value,
                found->second.trigger.test_value, 2, events)) {
            return SyncUpdate::resource_exhausted;
        }
    }
    if (!queue_events_atomically(events))
        return SyncUpdate::queue_full;
    sync_alarms_.erase(found);
    static_cast<void>(resources_.erase(id));
    return SyncUpdate::updated;
}

SyncFenceRecord *
ServerState::sync_fence(std::uint32_t id)
{
    const auto found = sync_fences_.find(id);
    return found == sync_fences_.end() ? nullptr : &found->second;
}

const SyncFenceRecord *
ServerState::sync_fence(std::uint32_t id) const
{
    const auto found = sync_fences_.find(id);
    return found == sync_fences_.end() ? nullptr : &found->second;
}

bool
ServerState::add_sync_fence(SyncFenceRecord fence, std::uint32_t owner)
{
    const std::uint32_t id = fence.id;
    if (!resources_.insert(id, ResourceKind::sync_fence, owner))
        return false;
    try {
        if (!sync_fences_.emplace(id, std::move(fence)).second) {
            static_cast<void>(resources_.erase(id));
            return false;
        }
    }
    catch (const std::bad_alloc &) {
        static_cast<void>(resources_.erase(id));
        return false;
    }
    return true;
}

SyncUpdate
ServerState::trigger_sync_fence(std::uint32_t id)
{
    auto *fence = sync_fence(id);
    if (fence == nullptr)
        return SyncUpdate::invalid;
    fence->triggered = true;
    for (auto iterator = sync_fence_waits_.begin();
         iterator != sync_fence_waits_.end();) {
        if (std::find(iterator->second.begin(), iterator->second.end(), id) !=
            iterator->second.end()) {
            iterator = sync_fence_waits_.erase(iterator);
        }
        else {
            ++iterator;
        }
    }
    return SyncUpdate::updated;
}

bool
ServerState::reset_sync_fence(std::uint32_t id) noexcept
{
    auto *fence = sync_fence(id);
    if (fence == nullptr || !fence->triggered)
        return false;
    fence->triggered = false;
    return true;
}

SyncUpdate
ServerState::erase_sync_fence(std::uint32_t id)
{
    if (sync_fence(id) == nullptr)
        return SyncUpdate::invalid;
    for (auto iterator = sync_fence_waits_.begin();
         iterator != sync_fence_waits_.end();) {
        if (std::find(iterator->second.begin(), iterator->second.end(), id) !=
            iterator->second.end()) {
            iterator = sync_fence_waits_.erase(iterator);
        }
        else {
            ++iterator;
        }
    }
    sync_fences_.erase(id);
    static_cast<void>(resources_.erase(id));
    return SyncUpdate::updated;
}

SyncUpdate
ServerState::begin_sync_counter_await(
    std::uint32_t client, std::vector<SyncWaitCondition> conditions)
{
    if (conditions.empty() ||
        conditions.size() > maximum_sync_wait_conditions ||
        sync_waiting(client)) {
        return SyncUpdate::invalid;
    }
    bool fired = false;
    for (const auto &condition : conditions) {
        const auto *counter = sync_counter(condition.trigger.counter);
        if (counter == nullptr)
            return SyncUpdate::invalid;
        if (sync_trigger_fired(
                condition.trigger, counter->value, counter->value)) {
            fired = true;
        }
    }
    if (fired) {
        std::vector<PlannedEvent> events;
        try {
            std::vector<SyncCounterNotifyEvent> notifications;
            notifications.reserve(conditions.size());
            for (const auto &condition : conditions) {
                const auto *counter = sync_counter(condition.trigger.counter);
                if (counter != nullptr &&
                    sync_notification_eligible(condition, counter->value)) {
                    notifications.push_back(SyncCounterNotifyEvent{
                        counter->id, condition.trigger.test_value,
                        counter->value, current_time_, 0, false});
                }
            }
            for (std::size_t index = 0; index < notifications.size();
                 ++index) {
                notifications[index].count = static_cast<std::uint16_t>(
                    notifications.size() - index - 1);
                events.emplace_back(client, notifications[index]);
            }
        }
        catch (const std::bad_alloc &) {
            return SyncUpdate::resource_exhausted;
        }
        return queue_events_atomically(events) ? SyncUpdate::updated
                                               : SyncUpdate::queue_full;
    }
    try {
        sync_counter_waits_.emplace(client, std::move(conditions));
    }
    catch (const std::bad_alloc &) {
        return SyncUpdate::resource_exhausted;
    }
    return SyncUpdate::updated;
}

SyncUpdate
ServerState::begin_sync_fence_await(
    std::uint32_t client, std::vector<std::uint32_t> fences)
{
    if (fences.empty() || fences.size() > maximum_sync_wait_conditions ||
        sync_waiting(client)) {
        return SyncUpdate::invalid;
    }
    for (const auto id : fences) {
        const auto *fence = sync_fence(id);
        if (fence == nullptr)
            return SyncUpdate::invalid;
        if (fence->triggered)
            return SyncUpdate::updated;
    }
    try {
        sync_fence_waits_.emplace(client, std::move(fences));
    }
    catch (const std::bad_alloc &) {
        return SyncUpdate::resource_exhausted;
    }
    return SyncUpdate::updated;
}

bool
ServerState::sync_waiting(std::uint32_t client) const noexcept
{
    return sync_counter_waits_.find(client) != sync_counter_waits_.end() ||
        sync_fence_waits_.find(client) != sync_fence_waits_.end();
}

void
ServerState::set_sync_priority(std::uint32_t client, std::int32_t priority)
{
    const auto found = sync_priorities_.find(client);
    if (found != sync_priorities_.end())
        found->second = priority;
}

std::int32_t
ServerState::sync_priority(std::uint32_t client) const noexcept
{
    const auto found = sync_priorities_.find(client);
    return found == sync_priorities_.end() ? 0 : found->second;
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

std::uint32_t
ServerState::deepest_window_at(std::uint32_t parent_id,
                               std::int32_t x, std::int32_t y) const
{
    if (window(parent_id) == nullptr)
        return 0;
    const std::uint32_t child = child_window_at(parent_id, x, y);
    return child == 0 ? parent_id : deepest_window_at(child, x, y);
}

EventDelivery
ServerState::route_input_event(CoreInputEvent event, std::uint32_t mask,
                               std::uint32_t source,
                               std::uint32_t propagation_stop,
                               std::uint32_t pointer_window,
                               const ActiveGrab *grab,
                               std::vector<PlannedEvent> &events) const
{
    const auto deliver = [&](std::uint32_t destination,
                             const auto &recipients,
                             CoreInputEvent routed) {
        const auto origin = absolute_position(destination);
        routed.event = destination;
        routed.event_x = wire_coordinate(
            static_cast<std::int32_t>(routed.root_x) - origin.first);
        routed.event_y = wire_coordinate(
            static_cast<std::int32_t>(routed.root_y) - origin.second);
        routed.child = 0;
        std::uint32_t child = pointer_window;
        while (child != 0 && child != destination) {
            const auto *candidate = window(child);
            if (candidate == nullptr)
                break;
            if (candidate->parent == destination) {
                routed.child = child;
                break;
            }
            child = candidate->parent;
        }
        const std::size_t initial_size = events.size();
        try {
            for (const auto recipient : recipients)
                events.emplace_back(recipient, routed);
        }
        catch (const std::bad_alloc &) {
            events.resize(initial_size);
            return EventDelivery::queue_full;
        }
        return EventDelivery::delivered;
    };

    const auto forced = [&]() {
        if (grab == nullptr || (grab->event_mask & mask) == 0)
            return EventDelivery::no_recipient;
        return deliver(grab->window,
                       std::array<std::uint32_t, 1>{grab->owner}, event);
    };
    if (grab != nullptr && !grab->owner_events)
        return forced();

    auto *candidate = window(source);
    std::uint32_t propagated_mask = mask;
    while (candidate != nullptr) {
        std::vector<std::uint32_t> recipients;
        try {
            for (const auto &selection : candidate->event_masks) {
                if ((selection.second & propagated_mask) != 0 &&
                    (grab == nullptr || selection.first == grab->owner)) {
                    recipients.push_back(selection.first);
                }
            }
        }
        catch (const std::bad_alloc &) {
            return EventDelivery::queue_full;
        }
        if (!recipients.empty())
            return deliver(candidate->id, recipients, event);
        if (candidate->id == propagation_stop || candidate->parent == 0)
            break;
        propagated_mask &= ~candidate->do_not_propagate_mask;
        if (propagated_mask == 0)
            break;
        candidate = window(candidate->parent);
    }
    return forced();
}

EventDelivery
ServerState::append_crossing_events(
    std::uint32_t from, std::uint32_t to,
    std::int32_t root_x, std::int32_t root_y, std::uint16_t state,
    std::uint8_t mode, const ActiveGrab *grab, const FocusState &focus,
    std::vector<PlannedEvent> &events) const
{
    if (from == to)
        return EventDelivery::no_recipient;

    bool delivered = false;
    const auto append = [&](std::uint8_t type, std::uint8_t detail,
                            std::uint32_t destination,
                            std::uint32_t child) {
        const auto *target = window(destination);
        if (target == nullptr)
            return EventDelivery::no_recipient;
        CrossingEvent event;
        event.type = type;
        event.detail = detail;
        event.time = current_time_;
        event.root = root_window_id;
        event.event = destination;
        event.child = child;
        event.root_x = wire_coordinate(root_x);
        event.root_y = wire_coordinate(root_y);
        const auto origin = absolute_position(destination);
        event.event_x = wire_coordinate(root_x - origin.first);
        event.event_y = wire_coordinate(root_y - origin.second);
        event.state = state;
        event.mode = mode;
        event.focus = focus.kind == FocusKind::pointer_root ||
            (focus.kind == FocusKind::window &&
             (destination == focus.window ||
              is_descendant(destination, focus.window)));
        const std::uint32_t mask = type == 7 ? 1U << 4 : 1U << 5;
        const std::size_t initial_size = events.size();
        try {
            if (grab != nullptr) {
                bool selected = destination == grab->window &&
                    (grab->event_mask & mask) != 0;
                if (grab->owner_events) {
                    const auto selection = target->event_masks.find(
                        grab->owner);
                    selected = selected ||
                        (selection != target->event_masks.end() &&
                         (selection->second & mask) != 0);
                }
                if (selected)
                    events.emplace_back(grab->owner, event);
            }
            else {
                for (const auto &selection : target->event_masks) {
                    if ((selection.second & mask) != 0)
                        events.emplace_back(selection.first, event);
                }
            }
        }
        catch (const std::bad_alloc &) {
            events.resize(initial_size);
            return EventDelivery::queue_full;
        }
        return events.size() == initial_size
            ? EventDelivery::no_recipient
            : EventDelivery::delivered;
    };
    const auto add = [&](std::uint8_t type, std::uint8_t detail,
                         std::uint32_t destination,
                         std::uint32_t child) {
        const EventDelivery result = append(type, detail, destination, child);
        if (result == EventDelivery::delivered)
            delivered = true;
        return result != EventDelivery::queue_full;
    };

    if (is_descendant(to, from)) {
        if (!add(8, 2, from, 0)) // LeaveNotify, NotifyInferior
            return EventDelivery::queue_full;
        std::vector<std::uint32_t> path;
        try {
            for (std::uint32_t current = to; current != from;) {
                path.push_back(current);
                const auto *candidate = window(current);
                if (candidate == nullptr)
                    return EventDelivery::queue_full;
                current = candidate->parent;
            }
        }
        catch (const std::bad_alloc &) {
            return EventDelivery::queue_full;
        }
        std::reverse(path.begin(), path.end());
        for (std::size_t index = 0; index + 1 < path.size(); ++index) {
            if (!add(7, 1, path[index], path[index + 1]))
                return EventDelivery::queue_full;
        }
        if (!add(7, 0, to, 0)) // EnterNotify, NotifyAncestor
            return EventDelivery::queue_full;
        return delivered ? EventDelivery::delivered
                         : EventDelivery::no_recipient;
    }

    if (is_descendant(from, to)) {
        if (!add(8, 0, from, 0)) // LeaveNotify, NotifyAncestor
            return EventDelivery::queue_full;
        std::uint32_t child = from;
        for (const auto *candidate = window(from);
             candidate != nullptr && candidate->parent != to;) {
            candidate = window(candidate->parent);
            if (candidate == nullptr)
                return EventDelivery::queue_full;
            if (!add(8, 1, candidate->id, child))
                return EventDelivery::queue_full;
            child = candidate->id;
        }
        if (!add(7, 2, to, 0)) // EnterNotify, NotifyInferior
            return EventDelivery::queue_full;
        return delivered ? EventDelivery::delivered
                         : EventDelivery::no_recipient;
    }

    std::uint32_t common = from;
    while (common != 0 && common != to && !is_descendant(to, common)) {
        const auto *candidate = window(common);
        common = candidate == nullptr ? 0 : candidate->parent;
    }
    if (common == 0)
        return EventDelivery::queue_full;
    if (!add(8, 3, from, 0)) // LeaveNotify, NotifyNonlinear
        return EventDelivery::queue_full;
    std::uint32_t child = from;
    for (const auto *candidate = window(from);
         candidate != nullptr && candidate->parent != common;) {
        candidate = window(candidate->parent);
        if (candidate == nullptr)
            return EventDelivery::queue_full;
        if (!add(8, 4, candidate->id, child))
            return EventDelivery::queue_full;
        child = candidate->id;
    }
    std::vector<std::uint32_t> path;
    try {
        for (std::uint32_t current = to; current != common;) {
            path.push_back(current);
            const auto *candidate = window(current);
            if (candidate == nullptr)
                return EventDelivery::queue_full;
            current = candidate->parent;
        }
    }
    catch (const std::bad_alloc &) {
        return EventDelivery::queue_full;
    }
    std::reverse(path.begin(), path.end());
    for (std::size_t index = 0; index + 1 < path.size(); ++index) {
        if (!add(7, 4, path[index], path[index + 1]))
            return EventDelivery::queue_full;
    }
    if (!add(7, 3, to, 0)) // EnterNotify, NotifyNonlinear
        return EventDelivery::queue_full;
    return delivered ? EventDelivery::delivered
                     : EventDelivery::no_recipient;
}

EventDelivery
ServerState::append_focus_events(
    const FocusState &from, const FocusState &to, std::uint8_t mode,
    std::uint32_t pointer_window, std::vector<PlannedEvent> &events) const
{
    const bool same_focus = from.kind == to.kind &&
        (from.kind != FocusKind::window || from.window == to.window);
    if (same_focus && mode != 1 && mode != 2)
        return EventDelivery::no_recipient;

    bool delivered = false;
    const auto append = [&](std::uint8_t type, std::uint8_t detail,
                            std::uint32_t destination) {
        const auto *target = window(destination);
        if (target == nullptr)
            return EventDelivery::queue_full;
        const std::size_t initial_size = events.size();
        try {
            FocusEvent event;
            event.type = type;
            event.detail = detail;
            event.event = destination;
            event.mode = mode;
            for (const auto &selection : target->event_masks) {
                if ((selection.second & (1U << 21)) != 0)
                    events.emplace_back(selection.first, event);
            }
        }
        catch (const std::bad_alloc &) {
            events.resize(initial_size);
            return EventDelivery::queue_full;
        }
        return events.size() == initial_size
            ? EventDelivery::no_recipient
            : EventDelivery::delivered;
    };
    const auto add = [&](std::uint8_t type, std::uint8_t detail,
                         std::uint32_t destination) {
        const EventDelivery result = append(type, detail, destination);
        if (result == EventDelivery::delivered)
            delivered = true;
        return result != EventDelivery::queue_full;
    };
    const auto special_detail = [](FocusKind kind) {
        return static_cast<std::uint8_t>(
            kind == FocusKind::pointer_root ? 6 : 7);
    };
    if (same_focus) {
        if (from.kind != FocusKind::window)
            return EventDelivery::no_recipient;
        if (!add(10, 3, from.window) || !add(9, 3, from.window))
            return EventDelivery::queue_full;
        return delivered ? EventDelivery::delivered
                         : EventDelivery::no_recipient;
    }
    const auto pointer_related = [&](std::uint32_t left,
                                     std::uint32_t right) {
        return is_descendant(left, right) || is_descendant(right, left);
    };
    const auto pointer_out = [&](std::uint32_t parent,
                                 std::uint32_t exclude, bool inclusive) {
        if (pointer_window != parent &&
            !is_descendant(pointer_window, parent)) {
            return true;
        }
        if (exclude != 0 && pointer_related(pointer_window, exclude))
            return true;
        const auto *parent_window = window(parent);
        if (parent_window == nullptr)
            return false;
        const std::uint32_t stop = inclusive
            ? parent_window->parent
            : parent;
        for (std::uint32_t current = pointer_window; current != stop;) {
            if (!add(10, 5, current)) // FocusOut, NotifyPointer
                return false;
            const auto *candidate = window(current);
            if (candidate == nullptr)
                return false;
            current = candidate->parent;
        }
        return true;
    };
    const auto pointer_in = [&](std::uint32_t parent,
                                std::uint32_t exclude, bool inclusive) {
        if (pointer_window == exclude ||
            (pointer_window != parent &&
             !is_descendant(pointer_window, parent))) {
            return true;
        }
        if (exclude != 0 && pointer_related(pointer_window, exclude))
            return true;
        std::vector<std::uint32_t> path;
        try {
            for (std::uint32_t current = pointer_window;;) {
                if (!inclusive && current == parent)
                    break;
                path.push_back(current);
                if (current == parent)
                    break;
                const auto *candidate = window(current);
                if (candidate == nullptr)
                    return false;
                current = candidate->parent;
            }
        }
        catch (const std::bad_alloc &) {
            return false;
        }
        std::reverse(path.begin(), path.end());
        for (const auto destination : path) {
            if (!add(9, 5, destination)) // FocusIn, NotifyPointer
                return false;
        }
        return true;
    };

    if (from.kind != FocusKind::window &&
        to.kind != FocusKind::window) {
        if (from.kind == FocusKind::pointer_root &&
            !pointer_out(root_window_id, 0, true)) {
            return EventDelivery::queue_full;
        }
        if (!add(10, special_detail(from.kind), root_window_id) ||
            !add(9, special_detail(to.kind), root_window_id)) {
            return EventDelivery::queue_full;
        }
        if (to.kind == FocusKind::pointer_root &&
            !pointer_in(root_window_id, 0, true)) {
            return EventDelivery::queue_full;
        }
        return delivered ? EventDelivery::delivered
                         : EventDelivery::no_recipient;
    }

    if (to.kind != FocusKind::window) {
        if (!pointer_out(from.window, 0, false))
            return EventDelivery::queue_full;
        if (!add(10, 3, from.window)) // FocusOut, NotifyNonlinear
            return EventDelivery::queue_full;
        for (const auto *candidate = window(from.window);
             candidate != nullptr && candidate->parent != 0;) {
            candidate = window(candidate->parent);
            if (candidate == nullptr || !add(10, 4, candidate->id))
                return EventDelivery::queue_full;
        }
        if (!add(9, special_detail(to.kind), root_window_id))
            return EventDelivery::queue_full;
        if (to.kind == FocusKind::pointer_root &&
            !pointer_in(root_window_id, 0, true)) {
            return EventDelivery::queue_full;
        }
        return delivered ? EventDelivery::delivered
                         : EventDelivery::no_recipient;
    }

    if (from.kind != FocusKind::window) {
        if (from.kind == FocusKind::pointer_root &&
            !pointer_out(root_window_id, 0, true)) {
            return EventDelivery::queue_full;
        }
        if (!add(10, special_detail(from.kind), root_window_id))
            return EventDelivery::queue_full;
        if (to.window != root_window_id) {
            if (!add(9, 4, root_window_id))
                return EventDelivery::queue_full;
            std::vector<std::uint32_t> path;
            try {
                for (std::uint32_t current = to.window;
                     current != root_window_id;) {
                    path.push_back(current);
                    const auto *candidate = window(current);
                    if (candidate == nullptr)
                        return EventDelivery::queue_full;
                    current = candidate->parent;
                }
            }
            catch (const std::bad_alloc &) {
                return EventDelivery::queue_full;
            }
            std::reverse(path.begin(), path.end());
            for (std::size_t index = 0; index + 1 < path.size(); ++index) {
                if (!add(9, 4, path[index]))
                    return EventDelivery::queue_full;
            }
        }
        if (!add(9, 3, to.window)) // FocusIn, NotifyNonlinear
            return EventDelivery::queue_full;
        if (!pointer_in(to.window, 0, false))
            return EventDelivery::queue_full;
        return delivered ? EventDelivery::delivered
                         : EventDelivery::no_recipient;
    }

    if (is_descendant(to.window, from.window)) {
        if (!pointer_out(from.window, to.window, false))
            return EventDelivery::queue_full;
        if (!add(10, 2, from.window)) // FocusOut, NotifyInferior
            return EventDelivery::queue_full;
        std::vector<std::uint32_t> path;
        try {
            for (std::uint32_t current = to.window;
                 current != from.window;) {
                path.push_back(current);
                const auto *candidate = window(current);
                if (candidate == nullptr)
                    return EventDelivery::queue_full;
                current = candidate->parent;
            }
        }
        catch (const std::bad_alloc &) {
            return EventDelivery::queue_full;
        }
        std::reverse(path.begin(), path.end());
        for (std::size_t index = 0; index + 1 < path.size(); ++index) {
            if (!add(9, 1, path[index]))
                return EventDelivery::queue_full;
        }
        if (!add(9, 0, to.window)) // FocusIn, NotifyAncestor
            return EventDelivery::queue_full;
        return delivered ? EventDelivery::delivered
                         : EventDelivery::no_recipient;
    }

    if (is_descendant(from.window, to.window)) {
        if (!add(10, 0, from.window)) // FocusOut, NotifyAncestor
            return EventDelivery::queue_full;
        for (const auto *candidate = window(from.window);
             candidate != nullptr && candidate->parent != to.window;) {
            candidate = window(candidate->parent);
            if (candidate == nullptr || !add(10, 1, candidate->id))
                return EventDelivery::queue_full;
        }
        if (!add(9, 2, to.window)) // FocusIn, NotifyInferior
            return EventDelivery::queue_full;
        if (!pointer_in(to.window, from.window, false))
            return EventDelivery::queue_full;
        return delivered ? EventDelivery::delivered
                         : EventDelivery::no_recipient;
    }

    std::uint32_t common = from.window;
    while (common != 0 && common != to.window &&
           !is_descendant(to.window, common)) {
        const auto *candidate = window(common);
        common = candidate == nullptr ? 0 : candidate->parent;
    }
    if (common == 0 || !pointer_out(from.window, 0, false) ||
        !add(10, 3, from.window)) {
        return EventDelivery::queue_full;
    }
    for (const auto *candidate = window(from.window);
         candidate != nullptr && candidate->parent != common;) {
        candidate = window(candidate->parent);
        if (candidate == nullptr || !add(10, 4, candidate->id))
            return EventDelivery::queue_full;
    }
    std::vector<std::uint32_t> path;
    try {
        for (std::uint32_t current = to.window; current != common;) {
            path.push_back(current);
            const auto *candidate = window(current);
            if (candidate == nullptr)
                return EventDelivery::queue_full;
            current = candidate->parent;
        }
    }
    catch (const std::bad_alloc &) {
        return EventDelivery::queue_full;
    }
    std::reverse(path.begin(), path.end());
    for (std::size_t index = 0; index + 1 < path.size(); ++index) {
        if (!add(9, 4, path[index]))
            return EventDelivery::queue_full;
    }
    if (!add(9, 3, to.window))
        return EventDelivery::queue_full;
    if (!pointer_in(to.window, 0, false))
        return EventDelivery::queue_full;
    return delivered ? EventDelivery::delivered
                     : EventDelivery::no_recipient;
}

void
ServerState::refresh_modifier_button_mask() noexcept
{
    std::uint16_t state = 0;
    for (std::size_t group = 0; group < 8; ++group) {
        for (std::size_t index = 0;
             index < input_.modifier_keys_per_group; ++index) {
            const std::uint8_t keycode = input_.modifier_map[
                group * input_.modifier_keys_per_group + index];
            if (keycode != 0 &&
                (input_.pressed_keys[keycode >> 3] &
                 (1U << (keycode & 7U))) != 0) {
                state |= static_cast<std::uint16_t>(1U << group);
                break;
            }
        }
    }
    for (std::size_t button = 1; button <= 5; ++button) {
        if (input_.pressed_buttons.test(button))
            state |= static_cast<std::uint16_t>(1U << (button + 7));
    }
    input_.modifier_button_mask = state;
}

EventDelivery
ServerState::repeat_key(std::uint8_t detail)
{
    const std::uint32_t pointer_window = deepest_window_at(
        root_window_id, input_.pointer_x, input_.pointer_y);
    std::uint32_t source = pointer_window;
    std::uint32_t propagation_stop = root_window_id;
    if (input_.focus.kind == FocusKind::none) {
        source = 0;
    }
    else if (input_.focus.kind == FocusKind::window) {
        propagation_stop = input_.focus.window;
        if (pointer_window != input_.focus.window &&
            !is_descendant(pointer_window, input_.focus.window)) {
            source = input_.focus.window;
        }
    }

    std::uint16_t press_state = input_.modifier_button_mask;
    for (std::size_t group = 0; group < 8; ++group) {
        bool contains_key = false;
        bool another_pressed = false;
        for (std::size_t index = 0;
             index < input_.modifier_keys_per_group; ++index) {
            const std::uint8_t keycode = input_.modifier_map[
                group * input_.modifier_keys_per_group + index];
            contains_key = contains_key || keycode == detail;
            if (keycode != 0 && keycode != detail &&
                (input_.pressed_keys[keycode >> 3] &
                 (1U << (keycode & 7U))) != 0) {
                another_pressed = true;
            }
        }
        if (contains_key && !another_pressed) {
            press_state &= static_cast<std::uint16_t>(~(1U << group));
        }
    }

    std::vector<PlannedEvent> events;
    bool delivered = false;
    const ActiveGrab *grab = input_.keyboard_grab
        ? &*input_.keyboard_grab
        : nullptr;
    for (const auto &repeated :
         std::array<std::pair<std::uint8_t, std::uint16_t>, 2>{{
             {3, input_.modifier_button_mask}, {2, press_state}}}) {
        CoreInputEvent event;
        event.type = repeated.first;
        event.detail = detail;
        event.time = current_time_;
        event.root = root_window_id;
        event.root_x = wire_coordinate(input_.pointer_x);
        event.root_y = wire_coordinate(input_.pointer_y);
        event.state = repeated.second;
        const EventDelivery routed = source == 0
            ? EventDelivery::no_recipient
            : route_input_event(
                event, repeated.first == 2 ? 1U << 0 : 1U << 1,
                source, propagation_stop, pointer_window, grab, events);
        if (routed == EventDelivery::queue_full)
            return routed;
        delivered = delivered || routed == EventDelivery::delivered;
    }
    if (!queue_events_atomically(events))
        return EventDelivery::queue_full;
    return delivered ? EventDelivery::delivered
                     : EventDelivery::no_recipient;
}

void
ServerState::update_repeat_controls() noexcept
{
    if (!key_repeat_)
        return;
    const std::uint8_t key = key_repeat_->key;
    const bool pressed = (input_.pressed_keys[key >> 3] &
                          (1U << (key & 7U))) != 0;
    const bool enabled = input_.global_auto_repeat &&
        (input_.auto_repeats[key >> 3] & (1U << (key & 7U))) != 0;
    if (!pressed || !enabled)
        key_repeat_.reset();
}

int
ServerState::timer_timeout_milliseconds() const noexcept
{
    if (!key_repeat_)
        return -1;
    const auto remaining = key_repeat_->deadline - clock_.now();
    if (remaining <= Clock::time_point::duration::zero())
        return 0;
    auto milliseconds = std::chrono::duration_cast<
        std::chrono::milliseconds>(remaining);
    if (milliseconds < remaining)
        milliseconds += std::chrono::milliseconds{1};
    if (milliseconds.count() > std::numeric_limits<int>::max())
        return std::numeric_limits<int>::max();
    return static_cast<int>(milliseconds.count());
}

EventDelivery
ServerState::process_timers()
{
    update_repeat_controls();
    if (!key_repeat_)
        return EventDelivery::no_recipient;
    const auto now = clock_.now();
    bool delivered = false;
    constexpr std::size_t maximum_repeat_burst = 32;
    std::size_t repeated = 0;
    while (key_repeat_ && key_repeat_->deadline <= now &&
           repeated < maximum_repeat_burst) {
        const std::uint8_t key = key_repeat_->key;
        advance_time();
        const EventDelivery result = repeat_key(key);
        if (result == EventDelivery::queue_full) {
            key_repeat_->deadline = now + default_repeat_interval;
            return result;
        }
        delivered = delivered || result == EventDelivery::delivered;
        key_repeat_->deadline += default_repeat_interval;
        ++repeated;
    }
    if (key_repeat_ && key_repeat_->deadline <= now)
        key_repeat_->deadline = now + default_repeat_interval;
    return delivered ? EventDelivery::delivered
                     : EventDelivery::no_recipient;
}

EventDelivery
ServerState::inject_input(std::uint8_t type, std::uint8_t detail,
                          std::int32_t root_x, std::int32_t root_y)
{
    const bool key_event = type == 2 || type == 3;
    const bool button_event = type == 4 || type == 5;
    const bool motion_event = type == 6;
    const std::uint16_t state_before = input_.modifier_button_mask;
    const std::int32_t event_x = motion_event ? root_x : input_.pointer_x;
    const std::int32_t event_y = motion_event ? root_y : input_.pointer_y;
    const std::uint32_t pointer_window = deepest_window_at(
        root_window_id, event_x, event_y);
    const std::uint32_t previous_pointer_window = motion_event
        ? deepest_window_at(root_window_id,
                            input_.pointer_x, input_.pointer_y)
        : pointer_window;
    std::uint32_t source = pointer_window;
    std::uint32_t propagation_stop = root_window_id;
    if (key_event) {
        if (input_.focus.kind == FocusKind::none)
            source = 0;
        else if (input_.focus.kind == FocusKind::window) {
            propagation_stop = input_.focus.window;
            if (pointer_window != input_.focus.window &&
                !is_descendant(pointer_window, input_.focus.window)) {
                source = input_.focus.window;
            }
        }
    }

    std::uint8_t wire_detail = detail;
    if (button_event)
        wire_detail = input_.pointer_map[detail - 1];
    std::uint32_t mask = key_event
        ? (type == 2 ? 1U << 0 : 1U << 1)
        : (button_event
            ? (type == 4 ? 1U << 2 : 1U << 3)
            : 1U << 6);
    if (motion_event && input_.pressed_buttons.any()) {
        mask |= 1U << 13;
        for (std::size_t button = 1; button <= 5; ++button) {
            if (input_.pressed_buttons.test(button))
                mask |= 1U << (button + 7);
        }
    }

    std::optional<ActiveGrab> activated;
    const ActiveGrab *grab = key_event
        ? (input_.keyboard_grab ? &*input_.keyboard_grab : nullptr)
        : (input_.pointer_grab ? &*input_.pointer_grab : nullptr);
    if (grab == nullptr && (type == 2 || type == 4)) {
        const PassiveGrabKind kind = key_event
            ? PassiveGrabKind::key
            : PassiveGrabKind::button;
        for (const auto &passive : passive_grabs_) {
            const std::uint8_t grab_detail = button_event
                ? wire_detail
                : detail;
            if (passive.kind != kind ||
                !passive.details.test(grab_detail) ||
                !passive.modifiers.test(state_before & all_modifiers_mask) ||
                (source != passive.window &&
                 !is_descendant(source, passive.window))) {
                continue;
            }
            activated = ActiveGrab{
                passive.owner, passive.window, passive.confine_to,
                current_time_, passive.event_mask, passive.pointer_mode,
                passive.keyboard_mode, passive.owner_events, true,
                grab_detail};
            grab = &*activated;
            break;
        }
    }

    CoreInputEvent event;
    event.type = type;
    event.detail = motion_event ? 0 : wire_detail;
    event.time = current_time_;
    event.root = root_window_id;
    event.root_x = wire_coordinate(event_x);
    event.root_y = wire_coordinate(event_y);
    event.state = state_before;
    std::vector<PlannedEvent> events;
    EventDelivery crossing = EventDelivery::no_recipient;
    EventDelivery grab_crossing = EventDelivery::no_recipient;
    EventDelivery grab_focus = EventDelivery::no_recipient;
    if (motion_event && previous_pointer_window != pointer_window) {
        crossing = append_crossing_events(
            previous_pointer_window, pointer_window, event_x, event_y,
            state_before, 0, grab, input_.focus, events);
        if (crossing == EventDelivery::queue_full)
            return crossing;
    }
    if (type == 4 && activated) {
        std::uint16_t state_after_press = state_before;
        if (detail <= 5) {
            state_after_press |= static_cast<std::uint16_t>(
                1U << (detail + 7));
        }
        grab_crossing = append_crossing_events(
            pointer_window, activated->window, event_x, event_y,
            state_after_press, 1, nullptr, input_.focus, events);
        if (grab_crossing == EventDelivery::queue_full)
            return grab_crossing;
    }
    if (type == 2 && activated) {
        FocusState grabbed_focus = input_.focus;
        grabbed_focus.kind = FocusKind::window;
        grabbed_focus.window = activated->window;
        grab_focus = append_focus_events(
            input_.focus, grabbed_focus, 1, pointer_window, events);
        if (grab_focus == EventDelivery::queue_full)
            return grab_focus;
    }
    const std::size_t route_start = events.size();
    const EventDelivery routed = source == 0 ||
            (button_event && wire_detail == 0)
        ? EventDelivery::no_recipient
        : route_input_event(event, mask, source, propagation_stop,
                            pointer_window, grab, events);
    if (routed == EventDelivery::queue_full)
        return routed;
    std::optional<ActiveGrab> automatic;
    if (type == 4 && grab == nullptr && routed == EventDelivery::delivered) {
        for (std::size_t index = route_start; index < events.size(); ++index) {
            const auto *press = std::get_if<CoreInputEvent>(
                &events[index].second);
            if (press == nullptr)
                continue;
            const auto *target = window(press->event);
            if (target == nullptr)
                break;
            const auto selection = target->event_masks.find(
                events[index].first);
            if (selection == target->event_masks.end())
                break;
            automatic = ActiveGrab{
                events[index].first, press->event, 0, current_time_,
                selection->second, 1, 1,
                (selection->second & (1U << 24)) != 0,
                false, 0, true};
            std::uint16_t state_after_press = state_before;
            if (detail <= 5) {
                state_after_press |= static_cast<std::uint16_t>(
                    1U << (detail + 7));
            }
            grab_crossing = append_crossing_events(
                pointer_window, press->event, event_x, event_y,
                state_after_press, 1, nullptr, input_.focus, events);
            if (grab_crossing == EventDelivery::queue_full)
                return grab_crossing;
            break;
        }
    }
    const bool releases_pointer_grab = type == 5 && grab != nullptr &&
        (grab->passive || grab->automatic) &&
        input_.pressed_buttons.test(detail) &&
        input_.pressed_buttons.count() == 1;
    if (releases_pointer_grab) {
        std::uint16_t state_after_release = state_before;
        if (detail <= 5) {
            state_after_release &= static_cast<std::uint16_t>(
                ~(1U << (detail + 7)));
        }
        grab_crossing = append_crossing_events(
            grab->window, pointer_window, event_x, event_y,
            state_after_release, 2, nullptr, input_.focus, events);
        if (grab_crossing == EventDelivery::queue_full)
            return grab_crossing;
    }
    const bool releases_keyboard_grab = type == 3 && grab != nullptr &&
        grab->passive && grab->passive_detail == detail;
    if (releases_keyboard_grab) {
        FocusState grabbed_focus = input_.focus;
        grabbed_focus.kind = FocusKind::window;
        grabbed_focus.window = grab->window;
        grab_focus = append_focus_events(
            grabbed_focus, input_.focus, 2, pointer_window, events);
        if (grab_focus == EventDelivery::queue_full)
            return grab_focus;
    }
    if (!queue_events_atomically(events))
        return EventDelivery::queue_full;
    const EventDelivery delivered =
        crossing == EventDelivery::delivered ||
        grab_crossing == EventDelivery::delivered ||
        grab_focus == EventDelivery::delivered ||
        routed == EventDelivery::delivered
        ? EventDelivery::delivered
        : EventDelivery::no_recipient;

    if (key_event) {
        const std::uint8_t bit = static_cast<std::uint8_t>(
            1U << (detail & 7U));
        auto &keys = input_.pressed_keys[detail >> 3];
        if (type == 2)
            keys |= bit;
        else
            keys &= static_cast<std::uint8_t>(~bit);
    }
    else if (button_event) {
        input_.pressed_buttons.set(detail, type == 4);
    }
    else if (motion_event) {
        input_.pointer_x = root_x;
        input_.pointer_y = root_y;
    }
    refresh_modifier_button_mask();
    if (activated) {
        if (key_event) {
            input_.keyboard_grab = *activated;
            input_.keyboard_grab_time = activated->activated_at;
        }
        else {
            input_.pointer_grab = *activated;
            input_.pointer_grab_time = activated->activated_at;
        }
    }
    if (automatic) {
        input_.pointer_grab = *automatic;
        input_.pointer_grab_time = automatic->activated_at;
    }
    if (releases_keyboard_grab)
        input_.keyboard_grab.reset();
    if (type == 5 && input_.pointer_grab &&
        (input_.pointer_grab->passive ||
         input_.pointer_grab->automatic) &&
        input_.pressed_buttons.none()) {
        input_.pointer_grab.reset();
    }
    if (type == 2 && input_.global_auto_repeat &&
        (input_.auto_repeats[detail >> 3] &
         (1U << (detail & 7U))) != 0) {
        key_repeat_ = KeyRepeat{
            detail, clock_.now() + default_repeat_delay};
    }
    else if (type == 3 && key_repeat_ && key_repeat_->key == detail) {
        key_repeat_.reset();
    }
    return delivered;
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

EventDelivery
ServerState::destroy_window(std::uint32_t id)
{
    if (id == root_window_id)
        return EventDelivery::no_recipient;
    auto found = windows_.find(id);
    if (found == windows_.end())
        return EventDelivery::no_recipient;

    const EventDelivery delivery = set_window_mapped(found->second, false);
    if (delivery == EventDelivery::queue_full)
        return delivery;
    erase_window_tree(id);
    return delivery;
}

void
ServerState::erase_window_tree(std::uint32_t id) noexcept
{
    auto found = windows_.find(id);
    if (found == windows_.end() || id == root_window_id)
        return;

    while (!found->second.children.empty())
        erase_window_tree(found->second.children.back());

    revert_focus_from(id);
    if (input_.pointer_grab &&
        (input_.pointer_grab->window == id ||
         input_.pointer_grab->confine_to == id)) {
        input_.pointer_grab.reset();
    }
    if (input_.keyboard_grab && input_.keyboard_grab->window == id)
        input_.keyboard_grab.reset();
    passive_grabs_.erase(
        std::remove_if(
            passive_grabs_.begin(), passive_grabs_.end(),
            [id](const PassiveGrab &grab) {
                return grab.window == id || grab.confine_to == id;
            }),
        passive_grabs_.end());
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

EventDelivery
ServerState::destroy_subwindows(std::uint32_t id)
{
    auto *parent = window(id);
    if (parent == nullptr)
        return EventDelivery::no_recipient;
    const EventDelivery delivery = set_subwindows_mapped(id, false);
    if (delivery == EventDelivery::queue_full)
        return delivery;
    while (!parent->children.empty())
        erase_window_tree(parent->children.back());
    return delivery;
}

bool
ServerState::is_descendant(std::uint32_t candidate,
                           std::uint32_t ancestor) const
{
    const auto *current = window(candidate);
    while (current != nullptr && current->parent != 0) {
        if (current->parent == ancestor)
            return true;
        current = window(current->parent);
    }
    return false;
}

ReparentUpdate
ServerState::reparent_window(std::uint32_t id, std::uint32_t new_parent,
                             std::int16_t x, std::int16_t y)
{
    auto *candidate = window(id);
    auto *parent = window(new_parent);
    if (candidate == nullptr || parent == nullptr || id == root_window_id ||
        id == new_parent || is_descendant(new_parent, id)) {
        return ReparentUpdate::invalid;
    }
    const std::uint32_t old_parent_id = candidate->parent;
    auto *old_parent = window(old_parent_id);
    if (old_parent == nullptr)
        return ReparentUpdate::invalid;

    const bool same_parent = old_parent_id == new_parent;
    std::vector<std::uint32_t> old_children;
    std::vector<std::uint32_t> new_children;
    try {
        old_children = old_parent->children;
        old_children.erase(
            std::remove(old_children.begin(), old_children.end(), id),
            old_children.end());
        if (same_parent) {
            old_children.push_back(id);
        }
        else {
            new_children = parent->children;
            new_children.push_back(id);
        }
    }
    catch (const std::bad_alloc &) {
        return ReparentUpdate::queue_full;
    }

    const std::int16_t old_x = candidate->x;
    const std::int16_t old_y = candidate->y;
    const bool was_mapped = candidate->mapped;
    const std::uint32_t old_pointer_window = deepest_window_at(
        root_window_id, input_.pointer_x, input_.pointer_y);
    const FocusState old_focus = input_.focus;
    FocusState new_focus = old_focus;
    std::vector<PlannedEvent> events;
    EventDelivery focus = EventDelivery::no_recipient;
    EventDelivery pointer_ungrab = EventDelivery::no_recipient;
    EventDelivery keyboard_ungrab = EventDelivery::no_recipient;
    EventDelivery unmap_crossing = EventDelivery::no_recipient;
    EventDelivery map_crossing = EventDelivery::no_recipient;

    candidate->mapped = false;
    const std::uint32_t unmapped_pointer_window = deepest_window_at(
        root_window_id, input_.pointer_x, input_.pointer_y);
    if (was_mapped && old_focus.kind == FocusKind::window &&
        map_state(old_focus.window) != 2) {
        new_focus = reverted_focus_state();
    }
    const bool pointer_grab_lost = was_mapped && input_.pointer_grab &&
        (map_state(input_.pointer_grab->window) != 2 ||
         (input_.pointer_grab->confine_to != 0 &&
          map_state(input_.pointer_grab->confine_to) != 2));
    const bool keyboard_grab_lost = was_mapped && input_.keyboard_grab &&
        map_state(input_.keyboard_grab->window) != 2;
    if (pointer_grab_lost) {
        pointer_ungrab = append_crossing_events(
            input_.pointer_grab->window, old_pointer_window,
            input_.pointer_x, input_.pointer_y,
            input_.modifier_button_mask, 2, nullptr, old_focus, events);
    }
    if (keyboard_grab_lost &&
        pointer_ungrab != EventDelivery::queue_full) {
        FocusState grabbed_focus = old_focus;
        grabbed_focus.kind = FocusKind::window;
        grabbed_focus.window = input_.keyboard_grab->window;
        keyboard_ungrab = append_focus_events(
            grabbed_focus, old_focus, 2, old_pointer_window, events);
    }
    if (was_mapped && pointer_ungrab != EventDelivery::queue_full &&
        keyboard_ungrab != EventDelivery::queue_full &&
        (old_focus.kind != new_focus.kind ||
         old_focus.window != new_focus.window)) {
        focus = append_focus_events(
            old_focus, new_focus,
            input_.keyboard_grab && !keyboard_grab_lost ? 3 : 0,
            old_pointer_window, events);
    }
    if (was_mapped && pointer_ungrab != EventDelivery::queue_full &&
        keyboard_ungrab != EventDelivery::queue_full &&
        focus != EventDelivery::queue_full &&
        old_pointer_window != unmapped_pointer_window) {
        unmap_crossing = append_crossing_events(
            old_pointer_window, unmapped_pointer_window,
            input_.pointer_x, input_.pointer_y,
            input_.modifier_button_mask, 0,
            pointer_grab_lost ? nullptr :
                (input_.pointer_grab ? &*input_.pointer_grab : nullptr),
            new_focus, events);
    }

    old_parent->children.swap(old_children);
    if (!same_parent)
        parent->children.swap(new_children);
    candidate->parent = new_parent;
    candidate->x = x;
    candidate->y = y;
    candidate->mapped = was_mapped;
    const std::uint32_t new_pointer_window = deepest_window_at(
        root_window_id, input_.pointer_x, input_.pointer_y);
    if (was_mapped && pointer_ungrab != EventDelivery::queue_full &&
        keyboard_ungrab != EventDelivery::queue_full &&
        focus != EventDelivery::queue_full &&
        unmap_crossing != EventDelivery::queue_full &&
        unmapped_pointer_window != new_pointer_window) {
        map_crossing = append_crossing_events(
            unmapped_pointer_window, new_pointer_window,
            input_.pointer_x, input_.pointer_y,
            input_.modifier_button_mask, 0,
            pointer_grab_lost ? nullptr :
                (input_.pointer_grab ? &*input_.pointer_grab : nullptr),
            new_focus, events);
    }

    candidate->mapped = was_mapped;
    candidate->parent = old_parent_id;
    candidate->x = old_x;
    candidate->y = old_y;
    old_parent->children.swap(old_children);
    if (!same_parent)
        parent->children.swap(new_children);
    if (pointer_ungrab == EventDelivery::queue_full ||
        keyboard_ungrab == EventDelivery::queue_full ||
        focus == EventDelivery::queue_full ||
        unmap_crossing == EventDelivery::queue_full ||
        map_crossing == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return ReparentUpdate::queue_full;
    }

    old_parent->children.swap(old_children);
    if (!same_parent)
        parent->children.swap(new_children);
    candidate->parent = new_parent;
    candidate->x = x;
    candidate->y = y;
    input_.focus = new_focus;
    if (pointer_grab_lost)
        input_.pointer_grab.reset();
    if (keyboard_grab_lost)
        input_.keyboard_grab.reset();
    invalidate_scene();
    return ReparentUpdate::updated;
}

EventDelivery
ServerState::set_subwindows_mapped(std::uint32_t id, bool mapped)
{
    const auto *parent = window(id);
    if (parent == nullptr)
        return EventDelivery::no_recipient;
    std::vector<std::uint32_t> changed;
    try {
        changed.reserve(parent->children.size());
    }
    catch (const std::bad_alloc &) {
        return EventDelivery::queue_full;
    }
    for (const auto child : parent->children) {
        const auto *candidate = window(child);
        if (candidate != nullptr && candidate->mapped != mapped) {
            try {
                changed.push_back(child);
            }
            catch (const std::bad_alloc &) {
                return EventDelivery::queue_full;
            }
        }
    }
    if (changed.empty())
        return EventDelivery::no_recipient;
    return update_window_mappings(changed.data(), changed.size(), mapped);
}

FocusState
ServerState::reverted_focus_state(std::uint32_t unavailable) const noexcept
{
    FocusState focus = input_.focus;
    const auto viewable = [this, unavailable](std::uint32_t id) {
        return id != unavailable &&
            !is_descendant(id, unavailable) && map_state(id) == 2;
    };
    if (focus.kind != FocusKind::window || viewable(focus.window))
        return focus;
    if (focus.revert_to == 0) {
        focus.kind = FocusKind::none;
        focus.window = 0;
        return focus;
    }
    if (focus.revert_to == 1) {
        focus.kind = FocusKind::pointer_root;
        focus.window = 0;
        return focus;
    }

    const auto *candidate = window(focus.window);
    std::uint32_t parent = candidate == nullptr ? 0 : candidate->parent;
    while (parent != 0 && !viewable(parent)) {
        const auto *ancestor = window(parent);
        parent = ancestor == nullptr ? 0 : ancestor->parent;
    }
    focus.revert_to = 0;
    focus.window = parent;
    focus.kind = parent == 0 ? FocusKind::none : FocusKind::window;
    return focus;
}

void
ServerState::revert_focus_from(std::uint32_t unavailable) noexcept
{
    auto &focus = input_.focus;
    if (focus.kind != FocusKind::window ||
        (focus.window != unavailable &&
         !is_descendant(focus.window, unavailable))) {
        return;
    }
    focus = reverted_focus_state(unavailable);
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
    clients_.erase(
        std::remove_if(
            clients_.begin(), clients_.end(),
            [owner](const auto &entry) { return entry.first == owner; }),
        clients_.end());
    if (server_grab_owner_ == owner)
        server_grab_owner_ = 0;
    for (auto &window_entry : windows_)
        window_entry.second.event_masks.erase(owner);
    for (auto &window_entry : windows_) {
        auto &clients = window_entry.second.shape_event_clients;
        clients.erase(std::remove(clients.begin(), clients.end(), owner),
                      clients.end());
    }
    sync_counter_waits_.erase(owner);
    sync_fence_waits_.erase(owner);
    sync_priorities_.erase(owner);
    for (auto &entry : sync_alarms_) {
        auto &clients = entry.second.event_clients;
        clients.erase(std::remove(clients.begin(), clients.end(), owner),
                      clients.end());
    }
    const auto queued = event_queues_.find(owner);
    if (queued != event_queues_.end()) {
        pending_events_ -= queued->second.size();
        event_queues_.erase(queued);
    }
    if (input_.pointer_grab && input_.pointer_grab->owner == owner &&
        deactivate_pointer_grab() == EventDelivery::queue_full) {
        input_.pointer_grab.reset();
    }
    if (input_.keyboard_grab && input_.keyboard_grab->owner == owner &&
        deactivate_keyboard_grab() == EventDelivery::queue_full) {
        input_.keyboard_grab.reset();
    }
    passive_grabs_.erase(
        std::remove_if(
            passive_grabs_.begin(), passive_grabs_.end(),
            [owner](const PassiveGrab &grab) { return grab.owner == owner; }),
        passive_grabs_.end());
    for (auto &selection : selections_) {
        if (selection.second.client == owner) {
            selection.second.window = 0;
            selection.second.client = 0;
            selection.second.changed_at = current_time_;
        }
    }
    const auto alarms = resources_.owned_by(owner, ResourceKind::sync_alarm);
    for (const auto id : alarms) {
        if (erase_sync_alarm(id) != SyncUpdate::updated) {
            sync_alarms_.erase(id);
            static_cast<void>(resources_.erase(id));
        }
    }
    const auto counters =
        resources_.owned_by(owner, ResourceKind::sync_counter);
    for (const auto id : counters) {
        if (erase_sync_counter(id) == SyncUpdate::updated)
            continue;
        for (auto &entry : sync_alarms_) {
            if (entry.second.trigger.counter == id) {
                entry.second.trigger.counter = 0;
                entry.second.state = 1;
            }
        }
        for (auto iterator = sync_counter_waits_.begin();
             iterator != sync_counter_waits_.end();) {
            const bool references = std::any_of(
                iterator->second.begin(), iterator->second.end(),
                [id](const SyncWaitCondition &condition) {
                    return condition.trigger.counter == id;
                });
            if (references)
                iterator = sync_counter_waits_.erase(iterator);
            else
                ++iterator;
        }
        sync_counters_.erase(id);
        static_cast<void>(resources_.erase(id));
    }
    const auto fences = resources_.owned_by(owner, ResourceKind::sync_fence);
    for (const auto id : fences)
        static_cast<void>(erase_sync_fence(id));
    const auto contexts =
        resources_.owned_by(owner, ResourceKind::graphics_context);
    for (const auto id : contexts)
        static_cast<void>(erase_graphics_context(id));
    const auto pixmaps = resources_.owned_by(owner, ResourceKind::pixmap);
    for (const auto id : pixmaps)
        static_cast<void>(erase_pixmap(id));
    const auto colormaps = resources_.owned_by(owner, ResourceKind::colormap);
    for (const auto id : colormaps)
        static_cast<void>(erase_colormap(id));
    const auto windows = resources_.owned_by(owner, ResourceKind::window);
    for (const auto id : windows) {
        if (destroy_window(id) != EventDelivery::queue_full)
            continue;
        auto *candidate = window(id);
        if (candidate != nullptr)
            candidate->mapped = false;
        revert_focus_from(id);
        erase_window_tree(id);
    }
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

std::uint32_t
ServerState::child_window_at(std::uint32_t parent_id,
                             std::int32_t x, std::int32_t y) const
{
    const auto *parent = window(parent_id);
    if (parent == nullptr)
        return 0;
    for (auto iterator = parent->children.rbegin();
         iterator != parent->children.rend(); ++iterator) {
        const auto *child = window(*iterator);
        if (child == nullptr || map_state(child->id) != 2)
            continue;
        const auto origin = absolute_position(child->id);
        const std::int64_t border = child->border_width;
        if (x < origin.first - border || y < origin.second - border ||
            x >= origin.first + child->width + border ||
            y >= origin.second + child->height + border) {
            continue;
        }
        const std::int64_t local_x =
            static_cast<std::int64_t>(x) - origin.first;
        const std::int64_t local_y =
            static_cast<std::int64_t>(y) - origin.second;
        if ((child->shapes[0] &&
             !child->shapes[0]->contains(local_x, local_y)) ||
            (child->shapes[2] &&
             !child->shapes[2]->contains(local_x, local_y))) {
            continue;
        }
        return child->id;
    }
    return 0;
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
