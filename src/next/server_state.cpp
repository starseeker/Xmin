#include "xmin/next/server_state.hpp"

#include "xmin/next/checked.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace xmin::next {

struct ServerState::SurfaceBudget {
    std::size_t bytes = 0;
};

struct ServerState::ManagedSurface {
    ManagedSurface(Surface value, std::shared_ptr<SurfaceBudget> accounting)
        : surface(std::move(value)), budget(std::move(accounting))
    {}

    ~ManagedSurface()
    {
        budget->bytes -= surface.storage_bytes();
    }

    Surface surface;
    std::shared_ptr<SurfaceBudget> budget;
};

namespace {

std::uint32_t
pack_cursor_color(const RenderColor &color) noexcept
{
    return 0xff000000U |
        (static_cast<std::uint32_t>(color.red >> 8) << 16) |
        (static_cast<std::uint32_t>(color.green >> 8) << 8) |
        static_cast<std::uint32_t>(color.blue >> 8);
}

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

std::uint32_t
screen_millimetres(std::uint16_t pixels) noexcept
{
    const auto tenths = static_cast<std::uint32_t>(pixels) * 254U;
    return (tenths + 480U) / 960U;
}

void
update_automatic_randr_monitors(RandrState &state,
                                std::uint16_t screen_width,
                                std::uint16_t screen_height)
{
    const auto mode = state.modes.find(state.current_mode);
    for (auto &entry : state.monitors) {
        auto &monitor = entry.second;
        if (!monitor.automatic)
            continue;
        if (mode == state.modes.end()) {
            monitor.outputs.clear();
            monitor.x = 0;
            monitor.y = 0;
            monitor.width = 0;
            monitor.height = 0;
            monitor.millimetre_width = 0;
            monitor.millimetre_height = 0;
            continue;
        }
        monitor.outputs = {randr_output_id};
        monitor.x = state.crtc_x;
        monitor.y = state.crtc_y;
        monitor.width = mode->second.width;
        monitor.height = mode->second.height;
        monitor.millimetre_width = screen_width == 0
            ? 0
            : static_cast<std::uint32_t>(
                  static_cast<std::uint64_t>(mode->second.width) *
                  state.millimetre_width / screen_width);
        monitor.millimetre_height = screen_height == 0
            ? 0
            : static_cast<std::uint32_t>(
                  static_cast<std::uint64_t>(mode->second.height) *
                  state.millimetre_height / screen_height);
    }
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

void
CursorImage::recolor(RenderColor new_foreground,
                     RenderColor new_background) noexcept
{
    foreground = new_foreground;
    background = new_background;
    if (pixel_roles.size() != pixels.size())
        return;
    const std::uint32_t foreground_pixel = pack_cursor_color(foreground);
    const std::uint32_t background_pixel = pack_cursor_color(background);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        switch (pixel_roles[index]) {
        case 1:
            pixels[index] = background_pixel;
            break;
        case 2:
            pixels[index] = foreground_pixel;
            break;
        default:
            pixels[index] = 0;
            break;
        }
    }
}

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
    : width_(width), height_(height),
      surface_budget_(std::make_shared<SurfaceBudget>()), clock_(clock),
      present_epoch_(clock.now())
{
    input_.pointer_x = width / 2;
    input_.pointer_y = height / 2;

    WindowRecord root;
    root.id = root_window_id;
    root.width = width;
    root.height = height;
    root.mapped = true;
    auto root_surface = Surface::create(width, height, 24);
    auto composite_surface = Surface::create(width, height, 24);
    if (root_surface)
        root.surface = adopt_surface(std::move(*root_surface));
    if (composite_surface)
        composited_root_ = adopt_surface(std::move(*composite_surface));
    windows_.emplace(root.id, std::move(root));
    static_cast<void>(
        resources_.insert(root_window_id, ResourceKind::window, 0));
    static_cast<void>(
        resources_.insert(default_colormap_id, ResourceKind::colormap, 0));

    randr_.millimetre_width = screen_millimetres(width);
    randr_.millimetre_height = screen_millimetres(height);
    RandrModeInfo initial_mode;
    initial_mode.id = randr_initial_mode_id;
    initial_mode.width = width;
    initial_mode.height = height;
    initial_mode.dot_clock = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(
            static_cast<std::uint64_t>(width) * height * 60U,
            std::numeric_limits<std::uint32_t>::max()));
    initial_mode.hsync_start = width;
    initial_mode.hsync_end = width;
    initial_mode.htotal = width;
    initial_mode.vsync_start = height;
    initial_mode.vsync_end = height;
    initial_mode.vtotal = height;
    initial_mode.name = std::to_string(width) + "x" + std::to_string(height);
    initial_mode.built_in = true;
    randr_.modes.emplace(initial_mode.id, std::move(initial_mode));
    randr_.output_modes.push_back(randr_initial_mode_id);
    randr_.gamma_red.resize(256);
    for (std::size_t index = 0; index < randr_.gamma_red.size(); ++index) {
        randr_.gamma_red[index] = static_cast<std::uint16_t>(index * 257U);
    }
    randr_.gamma_green = randr_.gamma_red;
    randr_.gamma_blue = randr_.gamma_red;
    const AtomId monitor_name = atoms_.intern("XMIN-0");
    randr_.monitors.emplace(
        monitor_name,
        RandrMonitor{monitor_name, true, true, 0, 0, width, height,
                     randr_.millimetre_width, randr_.millimetre_height,
                     {randr_output_id}});
}

bool
ServerState::valid() const noexcept
{
    const auto *root = window(root_window_id);
    return root != nullptr && root->surface && composited_root_;
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
    if (added.surface &&
        surface_budget_->bytes > maximum_server_surface_bytes)
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
    invalidate_scene();
    return true;
}

std::shared_ptr<Surface>
ServerState::adopt_surface(Surface surface)
{
    return adopt_replacement_surface(std::move(surface), 0);
}

std::shared_ptr<Surface>
ServerState::adopt_replacement_surface(
    Surface surface, std::size_t released_bytes)
{
    const std::size_t bytes = surface.storage_bytes();
    const std::size_t credit = std::min(
        released_bytes, surface_budget_->bytes);
    if (bytes > maximum_server_surface_bytes ||
        surface_budget_->bytes - credit >
            maximum_server_surface_bytes - bytes) {
        return {};
    }
    surface_budget_->bytes += bytes;
    try {
        auto allocation = std::make_shared<ManagedSurface>(
            std::move(surface), surface_budget_);
        return std::shared_ptr<Surface>(allocation, &allocation->surface);
    }
    catch (const std::bad_alloc &) {
        surface_budget_->bytes -= bytes;
        return {};
    }
}

bool
ServerState::resize_window_surface(WindowRecord &candidate,
                                   std::uint16_t width,
                                   std::uint16_t height)
{
    if (!candidate.surface)
        return true;
    auto replacement = Surface::create(
        width, height, candidate.surface->depth());
    if (!replacement)
        return false;
    replacement->copy_from(
        *candidate.surface, 0, 0, 0, 0, width, height, 3, 0xffffffffU);
    const bool retained = std::any_of(
        pixmaps_.begin(), pixmaps_.end(),
        [&candidate](const auto &entry) {
            return entry.second.surface == candidate.surface;
        });
    auto managed = adopt_replacement_surface(
        std::move(*replacement),
        retained ? 0 : candidate.surface->storage_bytes());
    if (!managed)
        return false;
    replace_window_surface(candidate, std::move(managed));
    invalidate_scene();
    return true;
}

EventDelivery
ServerState::configure_window(
    WindowRecord &candidate, std::int16_t x, std::int16_t y,
    std::uint16_t width, std::uint16_t height,
    std::uint16_t border_width,
    std::optional<std::uint32_t> sibling,
    std::optional<std::uint8_t> stack_mode)
{
    auto *parent = window(candidate.parent);
    if (candidate.parent == 0 || parent == nullptr)
        return EventDelivery::no_recipient;

    std::optional<std::vector<std::uint32_t>> children;
    std::vector<PlannedEvent> events;
    try {
        if (stack_mode) {
            children = parent->children;
            children->erase(
                std::remove(children->begin(), children->end(), candidate.id),
                children->end());
            auto position = children->end();
            if (sibling) {
                position = std::find(
                    children->begin(), children->end(), *sibling);
                if (*stack_mode == 0 || *stack_mode == 2 ||
                    *stack_mode == 4) {
                    ++position;
                }
            }
            else if (*stack_mode == 1 || *stack_mode == 3) {
                position = children->begin();
            }
            children->insert(position, candidate.id);
        }
    }
    catch (const std::bad_alloc &) {
        return EventDelivery::queue_full;
    }
    if (!append_present_configure_events(
            candidate.id, x, y, width, height, events)) {
        return EventDelivery::queue_full;
    }

    std::shared_ptr<Surface> replacement;
    if (candidate.surface &&
        (candidate.width != width || candidate.height != height)) {
        auto surface = Surface::create(width, height,
                                       candidate.surface->depth());
        if (!surface)
            return EventDelivery::queue_full;
        surface->copy_from(*candidate.surface, 0, 0, 0, 0,
                           width, height, 3, 0xffffffffU);
        const bool retained = std::any_of(
            pixmaps_.begin(), pixmaps_.end(),
            [&candidate](const auto &entry) {
                return entry.second.surface == candidate.surface;
            });
        replacement = adopt_replacement_surface(
            std::move(*surface), retained
                ? 0
                : candidate.surface->storage_bytes());
        if (!replacement)
            return EventDelivery::queue_full;
    }

    const bool has_recipient = !events.empty();
    if (!queue_events_atomically(events))
        return EventDelivery::queue_full;

    if (replacement)
        replace_window_surface(candidate, std::move(replacement));
    if (children)
        parent->children = std::move(*children);
    candidate.x = x;
    candidate.y = y;
    candidate.width = width;
    candidate.height = height;
    candidate.border_width = border_width;
    invalidate_scene();
    return has_recipient
        ? EventDelivery::delivered
        : EventDelivery::no_recipient;
}

void
ServerState::replace_window_surface(
    WindowRecord &candidate, std::shared_ptr<Surface> replacement) noexcept
{
    for (auto &entry : render_pictures_) {
        auto *drawable = std::get_if<RenderDrawableSource>(
            &entry.second->source);
        if (drawable != nullptr && !drawable->pixmap &&
            drawable->drawable == candidate.id) {
            drawable->surface = replacement;
        }
    }
    candidate.surface = std::move(replacement);
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
    if (!added.surface || resource_exists(added.id) ||
        surface_budget_->bytes > maximum_server_surface_bytes) {
        return false;
    }
    const std::uint32_t id = added.id;
    if (!resources_.insert(id, ResourceKind::pixmap, owner))
        return false;
    if (!pixmaps_.emplace(id, std::move(added)).second) {
        static_cast<void>(resources_.erase(id));
        return false;
    }
    return true;
}

bool
ServerState::erase_pixmap(std::uint32_t id)
{
    const auto found = pixmaps_.find(id);
    if (found == pixmaps_.end())
        return false;
    for (auto iterator = damages_.begin(); iterator != damages_.end();) {
        if (iterator->second.drawable != id) {
            ++iterator;
            continue;
        }
        static_cast<void>(resources_.erase(iterator->first));
        iterator = damages_.erase(iterator);
    }
    pixmaps_.erase(found);
    static_cast<void>(resources_.erase(id));
    return true;
}

DamageRecord *
ServerState::damage(std::uint32_t id)
{
    const auto found = damages_.find(id);
    return found == damages_.end() ? nullptr : &found->second;
}

const DamageRecord *
ServerState::damage(std::uint32_t id) const
{
    const auto found = damages_.find(id);
    return found == damages_.end() ? nullptr : &found->second;
}

bool
ServerState::append_damage_notifications(
    const DamageRecord &record, const Region &reported,
    bool full_area, std::vector<PlannedEvent> &events) const
{
    const auto *surface = drawable_surface(record.drawable);
    if (surface == nullptr)
        return true;
    const auto position = window(record.drawable)
        ? absolute_position(record.drawable)
        : std::pair<std::int32_t, std::int32_t>{0, 0};
    const Rectangle complete{
        0, 0, surface->width(), surface->height()};
    const std::size_t rectangle_count = full_area
        ? 1
        : reported.rectangles().size();
    if (rectangle_count > maximum_pending_events - events.size())
        return false;
    try {
        events.reserve(events.size() + rectangle_count);
        for (std::size_t index = 0; index < rectangle_count; ++index) {
            const Rectangle &area = full_area
                ? complete
                : reported.rectangles()[index];
            events.emplace_back(
                record.owner,
                DamageNotifyEvent{
                    static_cast<std::uint8_t>(
                        record.level |
                        (index + 1 < rectangle_count ? 0x80U : 0U)),
                    record.drawable, record.id, current_time_,
                    wire_coordinate(area.x), wire_coordinate(area.y),
                    wire_size(area.width), wire_size(area.height),
                    wire_coordinate(position.first),
                    wire_coordinate(position.second),
                    surface->width(), surface->height()});
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    return true;
}

bool
ServerState::apply_damage(
    DamageRecord &record, const Region &added,
    std::vector<PlannedEvent> &events) const
{
    const auto *surface = drawable_surface(record.drawable);
    if (surface == nullptr)
        return true;
    Region bounds;
    if (!Region::canonicalize(
            {Rectangle{0, 0, surface->width(), surface->height()}}, bounds)) {
        return false;
    }
    Region clipped;
    if (!Region::combine(
            RegionOperation::intersect, added, bounds, clipped)) {
        return false;
    }
    if (clipped.empty())
        return true;

    Region reported;
    bool notify = false;
    bool full_area = false;
    if (record.level == 0) {
        reported = clipped;
        notify = true;
    }
    else if (record.level == 1) {
        if (!Region::combine(
                RegionOperation::subtract, clipped, record.accumulated,
                reported)) {
            return false;
        }
        notify = !reported.empty();
    }

    const bool was_empty = record.accumulated.empty();
    const Rectangle old_extents = record.accumulated.extents();
    Region accumulated;
    if (!Region::combine(
            RegionOperation::unite, record.accumulated, clipped,
            accumulated)) {
        return false;
    }
    record.accumulated = std::move(accumulated);

    if (record.level == 2) {
        const Rectangle extents = record.accumulated.extents();
        notify = old_extents.x != extents.x || old_extents.y != extents.y ||
            old_extents.width != extents.width ||
            old_extents.height != extents.height;
        if (notify && !Region::canonicalize({extents}, reported))
            return false;
    }
    else if (record.level == 3) {
        notify = was_empty;
        full_area = true;
        reported = record.accumulated;
    }
    return !notify || append_damage_notifications(
        record, reported, full_area, events);
}

DamageUpdate
ServerState::add_damage(DamageRecord added, std::uint32_t owner)
{
    if (damages_.size() >= maximum_damage_objects)
        return DamageUpdate::resource_exhausted;
    if (drawable_surface(added.drawable) == nullptr ||
        resource_exists(added.id)) {
        return DamageUpdate::invalid;
    }
    added.owner = owner;
    const std::uint32_t id = added.id;
    std::vector<PlannedEvent> events;
    if (window(added.drawable) != nullptr) {
        Region initial;
        const auto *surface = drawable_surface(added.drawable);
        if (!Region::canonicalize(
                {Rectangle{0, 0, surface->width(), surface->height()}},
                initial) ||
            !apply_damage(added, initial, events)) {
            return DamageUpdate::resource_exhausted;
        }
    }
    if (!resources_.insert(id, ResourceKind::damage, owner))
        return DamageUpdate::invalid;
    try {
        if (!damages_.emplace(id, std::move(added)).second) {
            static_cast<void>(resources_.erase(id));
            return DamageUpdate::invalid;
        }
    }
    catch (const std::bad_alloc &) {
        static_cast<void>(resources_.erase(id));
        return DamageUpdate::resource_exhausted;
    }
    if (!queue_events_atomically(events)) {
        damages_.erase(id);
        static_cast<void>(resources_.erase(id));
        return DamageUpdate::queue_full;
    }
    return DamageUpdate::updated;
}

bool
ServerState::erase_damage(std::uint32_t id)
{
    if (damages_.erase(id) == 0)
        return false;
    static_cast<void>(resources_.erase(id));
    return true;
}

DamageUpdate
ServerState::damage_drawable(std::uint32_t drawable, const Region *region)
{
    invalidate_scene();
    const auto *surface = drawable_surface(drawable);
    if (surface == nullptr)
        return DamageUpdate::invalid;
    if (damages_.empty())
        return DamageUpdate::updated;
    Region complete;
    if (region == nullptr &&
        !Region::canonicalize(
            {Rectangle{0, 0, surface->width(), surface->height()}},
            complete)) {
        return DamageUpdate::resource_exhausted;
    }
    const Region &added = region == nullptr ? complete : *region;
    std::vector<std::pair<std::uint32_t, DamageRecord>> revised;
    std::vector<PlannedEvent> events;
    try {
        revised.reserve(damages_.size());
        for (const auto &entry : damages_) {
            if (drawable_surface(entry.second.drawable) != surface)
                continue;
            DamageRecord candidate = entry.second;
            if (!apply_damage(candidate, added, events))
                return DamageUpdate::resource_exhausted;
            revised.emplace_back(entry.first, std::move(candidate));
        }
    }
    catch (const std::bad_alloc &) {
        return DamageUpdate::resource_exhausted;
    }
    if (!queue_events_atomically(events))
        return DamageUpdate::queue_full;
    for (auto &entry : revised)
        damages_.at(entry.first) = std::move(entry.second);
    return DamageUpdate::updated;
}

DamageUpdate
ServerState::subtract_damage(
    std::uint32_t id, const Region *repair, Region *parts)
{
    const auto found = damages_.find(id);
    if (found == damages_.end())
        return DamageUpdate::invalid;
    if (found->second.level == 0)
        return DamageUpdate::updated;
    try {
        DamageRecord candidate = found->second;
        Region revised_parts;
        if (repair != nullptr) {
            if (parts != nullptr &&
                !Region::combine(
                    RegionOperation::intersect, candidate.accumulated,
                    *repair, revised_parts)) {
                return DamageUpdate::resource_exhausted;
            }
            Region remaining;
            if (!Region::combine(
                    RegionOperation::subtract, candidate.accumulated,
                    *repair, remaining)) {
                return DamageUpdate::resource_exhausted;
            }
            candidate.accumulated = std::move(remaining);
        }
        else {
            if (parts != nullptr)
                revised_parts = candidate.accumulated;
            Region empty;
            if (!Region::canonicalize({}, empty))
                return DamageUpdate::resource_exhausted;
            candidate.accumulated = std::move(empty);
        }

        std::vector<PlannedEvent> events;
        if (!candidate.accumulated.empty()) {
            Region reported = candidate.accumulated;
            if (candidate.level == 2 &&
                !Region::canonicalize(
                    {candidate.accumulated.extents()}, reported)) {
                return DamageUpdate::resource_exhausted;
            }
            if (!append_damage_notifications(
                    candidate, reported, candidate.level == 3, events)) {
                return DamageUpdate::resource_exhausted;
            }
        }
        if (!queue_events_atomically(events))
            return DamageUpdate::queue_full;
        found->second = std::move(candidate);
        if (parts != nullptr)
            *parts = std::move(revised_parts);
        return DamageUpdate::updated;
    }
    catch (const std::bad_alloc &) {
        return DamageUpdate::resource_exhausted;
    }
}

DamageUpdate
ServerState::damage_render_picture(std::uint32_t picture)
{
    const auto *candidate = render_picture(picture);
    if (candidate == nullptr)
        return DamageUpdate::invalid;
    const auto *drawable = std::get_if<RenderDrawableSource>(
        &candidate->source);
    return drawable == nullptr
        ? DamageUpdate::invalid
        : damage_drawable(drawable->drawable);
}

bool
ServerState::composite_window_redirected(std::uint32_t id) const noexcept
{
    const auto *candidate = window(id);
    if (candidate == nullptr)
        return false;
    return std::any_of(
        composite_redirects_.begin(), composite_redirects_.end(),
        [candidate, id](const CompositeRedirect &redirect) {
            return (!redirect.subwindows && redirect.window == id) ||
                (redirect.subwindows &&
                 redirect.window == candidate->parent);
        });
}

bool
ServerState::composite_window_manually_redirected(
    std::uint32_t id) const noexcept
{
    const auto *candidate = window(id);
    if (candidate == nullptr)
        return false;
    return std::any_of(
        composite_redirects_.begin(), composite_redirects_.end(),
        [candidate, id](const CompositeRedirect &redirect) {
            return redirect.update == 1 &&
                ((!redirect.subwindows && redirect.window == id) ||
                 (redirect.subwindows &&
                  redirect.window == candidate->parent));
        });
}

CompositeUpdate
ServerState::redirect_window(
    std::uint32_t owner, std::uint32_t id, bool subwindows,
    std::uint8_t update)
{
    const auto *candidate = window(id);
    if (candidate == nullptr || update > 1 ||
        (!subwindows &&
         (id == root_window_id || candidate->surface == nullptr))) {
        return CompositeUpdate::invalid;
    }
    if (update == 1) {
        const bool conflict = std::any_of(
            composite_redirects_.begin(), composite_redirects_.end(),
            [this, candidate, id, subwindows](
                const CompositeRedirect &redirect) {
                if (redirect.update != 1)
                    return false;
                if (!subwindows) {
                    return (!redirect.subwindows && redirect.window == id) ||
                        (redirect.subwindows &&
                         redirect.window == candidate->parent);
                }
                if (redirect.subwindows && redirect.window == id)
                    return true;
                if (redirect.subwindows)
                    return false;
                const auto *redirected = window(redirect.window);
                return redirected != nullptr &&
                    redirected->parent == id;
            });
        if (conflict)
            return CompositeUpdate::access_denied;
    }
    if (composite_redirects_.size() >= maximum_composite_redirects)
        return CompositeUpdate::resource_exhausted;
    try {
        composite_redirects_.push_back(
            CompositeRedirect{owner, id, update, subwindows});
    }
    catch (const std::bad_alloc &) {
        return CompositeUpdate::resource_exhausted;
    }
    invalidate_scene();
    return CompositeUpdate::updated;
}

CompositeUpdate
ServerState::unredirect_window(
    std::uint32_t owner, std::uint32_t id, bool subwindows,
    std::uint8_t update)
{
    const auto found = std::find_if(
        composite_redirects_.begin(), composite_redirects_.end(),
        [owner, id, subwindows, update](const CompositeRedirect &redirect) {
            return redirect.owner == owner && redirect.window == id &&
                redirect.subwindows == subwindows &&
                redirect.update == update;
        });
    if (found == composite_redirects_.end())
        return CompositeUpdate::invalid;
    const std::size_t removed = static_cast<std::size_t>(
        std::distance(composite_redirects_.begin(), found));
    const auto remains_redirected = [this, removed](
                                          std::uint32_t window_id) {
        const auto *candidate = window(window_id);
        if (candidate == nullptr)
            return false;
        for (std::size_t index = 0;
             index < composite_redirects_.size(); ++index) {
            if (index == removed)
                continue;
            const auto &redirect = composite_redirects_[index];
            if ((!redirect.subwindows &&
                 redirect.window == window_id) ||
                (redirect.subwindows &&
                 redirect.window == candidate->parent)) {
                return true;
            }
        }
        return false;
    };
    std::vector<std::pair<std::uint32_t, std::shared_ptr<Surface>>>
        replacements;
    const auto prepare_replacement = [this, &replacements,
                                      &remains_redirected](
                                         std::uint32_t window_id) {
        auto *candidate = window(window_id);
        if (candidate == nullptr || !candidate->surface ||
            remains_redirected(window_id)) {
            return true;
        }
        const bool named = std::any_of(
            pixmaps_.begin(), pixmaps_.end(),
            [candidate](const auto &entry) {
                return entry.second.surface == candidate->surface;
            });
        if (!named)
            return true;
        auto copied = Surface::create(
            candidate->surface->width(), candidate->surface->height(),
            candidate->surface->depth());
        if (!copied)
            return false;
        copied->copy_from(
            *candidate->surface, 0, 0, 0, 0,
            candidate->surface->width(), candidate->surface->height(),
            3, 0xffffffffU);
        auto managed = adopt_surface(std::move(*copied));
        if (!managed)
            return false;
        try {
            replacements.emplace_back(window_id, std::move(managed));
        }
        catch (const std::bad_alloc &) {
            return false;
        }
        return true;
    };
    if (subwindows) {
        const auto *parent = window(id);
        if (parent == nullptr)
            return CompositeUpdate::invalid;
        for (const auto child : parent->children) {
            if (!prepare_replacement(child))
                return CompositeUpdate::resource_exhausted;
        }
    }
    else if (!prepare_replacement(id)) {
        return CompositeUpdate::resource_exhausted;
    }
    composite_redirects_.erase(found);
    for (auto &replacement : replacements) {
        auto *candidate = window(replacement.first);
        if (candidate != nullptr)
            replace_window_surface(*candidate, std::move(replacement.second));
    }
    invalidate_scene();
    return CompositeUpdate::updated;
}

CompositeUpdate
ServerState::name_window_pixmap(
    std::uint32_t window_id, std::uint32_t pixmap_id,
    std::uint32_t owner)
{
    const auto *candidate = window(window_id);
    if (candidate == nullptr || candidate->surface == nullptr ||
        map_state(window_id) != 2 ||
        !composite_window_redirected(window_id) ||
        resource_exists(pixmap_id)) {
        return CompositeUpdate::invalid;
    }
    if (resource_limit_reached(owner))
        return CompositeUpdate::resource_exhausted;
    return add_pixmap({pixmap_id, candidate->surface}, owner)
        ? CompositeUpdate::updated
        : CompositeUpdate::resource_exhausted;
}

std::uint64_t
ServerState::present_msc() const noexcept
{
    const auto elapsed = clock_.now() - present_epoch_;
    if (elapsed <= Clock::time_point::duration::zero())
        return 0;
    const auto microseconds = std::chrono::duration_cast<
        std::chrono::microseconds>(elapsed).count();
    if (microseconds <= 0)
        return 0;
    return static_cast<std::uint64_t>(microseconds) /
        static_cast<std::uint64_t>(present_refresh_interval.count());
}

std::uint64_t
ServerState::present_ust() const noexcept
{
    const auto elapsed = clock_.now() - present_epoch_;
    if (elapsed <= Clock::time_point::duration::zero())
        return 1;
    const auto microseconds = std::chrono::duration_cast<
        std::chrono::microseconds>(elapsed).count();
    if (microseconds < 0)
        return 1;
    const auto value = static_cast<std::uint64_t>(microseconds);
    return value == std::numeric_limits<std::uint64_t>::max()
        ? value
        : value + 1;
}

std::optional<std::uint64_t>
ServerState::present_target_msc(
    std::uint64_t requested, std::uint64_t divisor,
    std::uint64_t remainder, std::uint32_t options) const noexcept
{
    const std::uint64_t current = present_msc();
    if (requested > current)
        return requested;

    const bool asynchronous = (options & 1U) != 0;
    if (divisor == 0) {
        if (asynchronous)
            return current;
        if (current == std::numeric_limits<std::uint64_t>::max())
            return std::nullopt;
        return current + 1;
    }

    std::uint64_t target = current - current % divisor;
    if (remainder > std::numeric_limits<std::uint64_t>::max() - target)
        return std::nullopt;
    target += remainder;
    if (target > current)
        return target;
    if (asynchronous && target == current)
        return target;
    if (divisor > std::numeric_limits<std::uint64_t>::max() - target)
        return std::nullopt;
    return target + divisor;
}

PresentUpdate
ServerState::select_present_input(
    std::uint32_t owner, std::uint32_t id, std::uint32_t window_id,
    std::uint32_t mask)
{
    if ((mask & ~std::uint32_t{0x7}) != 0)
        return PresentUpdate::invalid;
    const auto found = std::find_if(
        present_subscriptions_.begin(), present_subscriptions_.end(),
        [id](const PresentSubscription &entry) { return entry.id == id; });
    if (found != present_subscriptions_.end()) {
        if (found->owner != owner || found->window != window_id)
            return PresentUpdate::match;
        if (mask != 0) {
            found->mask = mask;
        }
        else {
            present_subscriptions_.erase(found);
            static_cast<void>(resources_.erase(id));
        }
        return PresentUpdate::updated;
    }
    if (mask == 0)
        return PresentUpdate::updated;
    if (window(window_id) == nullptr ||
        !valid_client_resource(id, owner))
        return PresentUpdate::invalid;
    if (resource_limit_reached(owner))
        return PresentUpdate::resource_exhausted;
    if (present_subscriptions_.size() >= maximum_present_subscriptions)
        return PresentUpdate::resource_exhausted;
    if (!resources_.insert(id, ResourceKind::present_event, owner))
        return PresentUpdate::invalid;
    try {
        present_subscriptions_.push_back(
            PresentSubscription{id, owner, window_id, mask});
    }
    catch (const std::bad_alloc &) {
        static_cast<void>(resources_.erase(id));
        return PresentUpdate::resource_exhausted;
    }
    return PresentUpdate::updated;
}

bool
ServerState::present_wait_ready(
    const PresentOperation &operation) const noexcept
{
    if (operation.wait_fence == 0)
        return true;
    const auto *fence = sync_fence(operation.wait_fence);
    return fence == nullptr || fence->triggered;
}

bool
ServerState::append_present_complete_events(
    std::uint32_t window_id, std::uint32_t serial,
    std::uint8_t kind, std::uint64_t msc, std::uint64_t ust,
    std::vector<PlannedEvent> &events) const
{
    try {
        for (const auto &subscription : present_subscriptions_) {
            if (subscription.window != window_id ||
                (subscription.mask & (1U << 1)) == 0) {
                continue;
            }
            if (events.size() == maximum_pending_events)
                return false;
            events.emplace_back(
                subscription.owner,
                PresentCompleteNotifyEvent{
                    kind, 0, subscription.id, window_id, serial,
                    ust, msc});
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    return true;
}

PresentUpdate
ServerState::execute_present(
    PresentOperation &operation, std::uint64_t msc, std::uint64_t ust)
{
    auto *target = window(operation.window);
    if (target == nullptr ||
        (operation.kind == PresentKind::pixmap &&
         (!target->surface || !operation.pixmap_surface))) {
        if (operation.idle_fence != 0)
            static_cast<void>(trigger_sync_fence(operation.idle_fence));
        return PresentUpdate::updated;
    }

    std::vector<PlannedEvent> events;
    const auto kind = operation.kind == PresentKind::pixmap ? 0U : 1U;
    if (!append_present_complete_events(
            operation.window, operation.serial,
            static_cast<std::uint8_t>(kind), msc, ust, events)) {
        return PresentUpdate::resource_exhausted;
    }
    for (const auto &notify : operation.notifies) {
        if (window(notify.window) != nullptr &&
            !append_present_complete_events(
                notify.window, notify.serial,
                static_cast<std::uint8_t>(kind), msc, ust, events)) {
            return PresentUpdate::resource_exhausted;
        }
    }
    if (operation.kind == PresentKind::pixmap) {
        try {
            for (const auto &subscription : present_subscriptions_) {
                if (subscription.window != operation.window ||
                    (subscription.mask & (1U << 2)) == 0) {
                    continue;
                }
                if (events.size() == maximum_pending_events)
                    return PresentUpdate::queue_full;
                events.emplace_back(
                    subscription.owner,
                    PresentIdleNotifyEvent{
                        subscription.id, operation.window,
                        operation.serial, operation.pixmap,
                        operation.idle_fence});
            }
        }
        catch (const std::bad_alloc &) {
            return PresentUpdate::resource_exhausted;
        }
    }
    if (!queue_events_atomically(events))
        return PresentUpdate::queue_full;

    if (operation.kind == PresentKind::pixmap) {
        const auto width = operation.pixmap_surface->width();
        const auto height = operation.pixmap_surface->height();
        target->surface->copy_from(
            *operation.pixmap_surface, 0, 0,
            operation.x_off, operation.y_off, width, height,
            3, 0xffffffffU,
            ClipView{operation.update ? &*operation.update : nullptr,
                     operation.x_off, operation.y_off});
        if (operation.update) {
            Region changed = *operation.update;
            if (changed.translate(operation.x_off, operation.y_off))
                static_cast<void>(damage_drawable(operation.window, &changed));
            else
                static_cast<void>(damage_drawable(operation.window));
        }
        else {
            static_cast<void>(damage_drawable(operation.window));
        }
        if (operation.idle_fence != 0)
            static_cast<void>(trigger_sync_fence(operation.idle_fence));
    }
    return PresentUpdate::updated;
}

PresentUpdate
ServerState::submit_present(PresentOperation operation)
{
    if (present_operations_.size() >= maximum_present_operations ||
        operation.notifies.size() > maximum_present_notifies) {
        return PresentUpdate::resource_exhausted;
    }
    if ((operation.divisor == 0 && operation.remainder != 0) ||
        (operation.divisor != 0 &&
         operation.remainder >= operation.divisor)) {
        return PresentUpdate::invalid;
    }
    const auto target = present_target_msc(
        operation.target_msc, operation.divisor,
        operation.remainder, operation.options);
    if (!target)
        return PresentUpdate::invalid;
    operation.target_msc = *target;
    const std::uint64_t current = present_msc();
    if (operation.target_msc <= current && present_wait_ready(operation))
        return execute_present(operation, current, present_ust());
    try {
        present_operations_.push_back(std::move(operation));
    }
    catch (const std::bad_alloc &) {
        return PresentUpdate::resource_exhausted;
    }
    return PresentUpdate::updated;
}

EventDelivery
ServerState::present_window_configured(std::uint32_t window_id)
{
    const auto *candidate = window(window_id);
    if (candidate == nullptr)
        return EventDelivery::no_recipient;
    std::vector<PlannedEvent> events;
    if (!append_present_configure_events(
            window_id, candidate->x, candidate->y,
            candidate->width, candidate->height, events)) {
        return EventDelivery::queue_full;
    }
    if (events.empty())
        return EventDelivery::no_recipient;
    return queue_events_atomically(events)
        ? EventDelivery::delivered
        : EventDelivery::queue_full;
}

bool
ServerState::append_present_configure_events(
    std::uint32_t window_id, std::int16_t x, std::int16_t y,
    std::uint16_t width, std::uint16_t height,
    std::vector<PlannedEvent> &events) const
{
    try {
        for (const auto &subscription : present_subscriptions_) {
            if (subscription.window != window_id ||
                (subscription.mask & 1U) == 0) {
                continue;
            }
            events.emplace_back(
                subscription.owner,
                PresentConfigureNotifyEvent{
                    subscription.id, window_id, x, y, width, height,
                    0, 0, width, height, 0});
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }
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
        return candidate->surface.get();
    if (auto *candidate = pixmap(id))
        return candidate->surface.get();
    return nullptr;
}

const Surface *
ServerState::drawable_surface(std::uint32_t id) const
{
    if (const auto *candidate = window(id))
        return candidate->surface.get();
    if (const auto *candidate = pixmap(id))
        return candidate->surface.get();
    return nullptr;
}

std::uint8_t
ServerState::drawable_depth(std::uint32_t id) const
{
    const auto *surface = drawable_surface(id);
    return surface == nullptr ? 0 : surface->depth();
}

std::uint32_t
ServerState::pointer_window() const noexcept
{
    return deepest_window_at(
        root_window_id, input_.pointer_x, input_.pointer_y);
}

std::shared_ptr<CursorImage>
ServerState::effective_cursor(std::uint32_t id) const noexcept
{
    for (const auto *candidate = window(id); candidate != nullptr;
         candidate = window(candidate->parent)) {
        if (candidate->cursor)
            return candidate->cursor;
    }
    return {};
}

std::shared_ptr<CursorImage>
ServerState::current_cursor_for(
    std::uint32_t pointer, const ActiveGrab *pointer_grab) const noexcept
{
    if (pointer_grab == nullptr)
        return effective_cursor(pointer);
    if (pointer_grab->cursor)
        return pointer_grab->cursor;
    const std::uint32_t base =
        pointer == pointer_grab->window ||
            is_descendant(pointer, pointer_grab->window)
        ? pointer
        : pointer_grab->window;
    return effective_cursor(base);
}

std::shared_ptr<CursorImage>
ServerState::current_cursor() const noexcept
{
    return current_cursor_for(
        pointer_window(),
        input_.pointer_grab ? &*input_.pointer_grab : nullptr);
}

Surface *
ServerState::readable_surface(std::uint32_t id)
{
    if (id != root_window_id)
        return drawable_surface(id);
    composite_scene();
    return composited_root_.get();
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
    const auto prospective_cursor = current_cursor_for(
        new_pointer_window,
        pointer_grab_lost || !input_.pointer_grab
            ? nullptr
            : &*input_.pointer_grab);
    const EventDelivery cursor =
        pointer_ungrab == EventDelivery::queue_full ||
            keyboard_ungrab == EventDelivery::queue_full ||
            crossing == EventDelivery::queue_full ||
            focus == EventDelivery::queue_full
        ? EventDelivery::queue_full
        : append_cursor_change(prospective_cursor, events);
    for (std::size_t index = 0; index < count; ++index) {
        auto *candidate = window(changed[index]);
        if (candidate != nullptr && candidate->id != root_window_id)
            candidate->mapped = !mapped;
    }
    if (pointer_ungrab == EventDelivery::queue_full ||
        keyboard_ungrab == EventDelivery::queue_full ||
        crossing == EventDelivery::queue_full ||
        focus == EventDelivery::queue_full ||
        cursor == EventDelivery::queue_full ||
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
    displayed_cursor_ = prospective_cursor;
    invalidate_scene();
    return pointer_ungrab == EventDelivery::delivered ||
            keyboard_ungrab == EventDelivery::delivered ||
            crossing == EventDelivery::delivered ||
            focus == EventDelivery::delivered ||
            cursor == EventDelivery::delivered
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
    if (composite_window_manually_redirected(id))
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
    const auto prospective_cursor = current_cursor_for(pointer_window, &grab);
    const EventDelivery cursor = crossing == EventDelivery::queue_full
        ? EventDelivery::queue_full
        : append_cursor_change(prospective_cursor, events);
    if (crossing == EventDelivery::queue_full ||
        cursor == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return EventDelivery::queue_full;
    }
    input_.pointer_grab_time = grab.activated_at;
    input_.pointer_grab = std::move(grab);
    displayed_cursor_ = prospective_cursor;
    return cursor == EventDelivery::delivered
        ? cursor
        : crossing;
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
    const auto prospective_cursor = current_cursor_for(pointer_window, nullptr);
    const EventDelivery cursor = crossing == EventDelivery::queue_full
        ? EventDelivery::queue_full
        : append_cursor_change(prospective_cursor, events);
    if (crossing == EventDelivery::queue_full ||
        cursor == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return EventDelivery::queue_full;
    }
    input_.pointer_grab.reset();
    displayed_cursor_ = prospective_cursor;
    return cursor == EventDelivery::delivered
        ? cursor
        : crossing;
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
ServerState::append_randr_events(
    const RandrState &candidate, std::uint16_t width,
    std::uint16_t height, std::uint16_t notify_mask,
    AtomId property, std::uint8_t property_status,
    std::vector<PlannedEvent> &events) const
{
    const auto mode = candidate.modes.find(candidate.current_mode);
    const std::uint16_t crtc_width = mode == candidate.modes.end()
        ? 0
        : mode->second.width;
    const std::uint16_t crtc_height = mode == candidate.modes.end()
        ? 0
        : mode->second.height;
    try {
        for (const auto &subscription : candidate.subscriptions) {
            const std::uint16_t selected = subscription.mask & notify_mask;
            if ((selected & 1U) != 0) {
                events.emplace_back(
                    subscription.client,
                    RandrScreenChangeNotifyEvent{
                        candidate.timestamp, candidate.config_timestamp,
                        subscription.window, width, height,
                        wire_size(candidate.millimetre_width),
                        wire_size(candidate.millimetre_height),
                        candidate.rotation});
            }
            if ((selected & 2U) != 0) {
                RandrNotifyEvent event;
                event.subtype = 0;
                event.timestamp = candidate.timestamp;
                event.window = subscription.window;
                event.crtc = randr_crtc_id;
                event.mode = candidate.current_mode;
                event.x = candidate.current_mode == 0 ? 0 : candidate.crtc_x;
                event.y = candidate.current_mode == 0 ? 0 : candidate.crtc_y;
                event.width = crtc_width;
                event.height = crtc_height;
                event.rotation = candidate.rotation;
                events.emplace_back(subscription.client, std::move(event));
            }
            if ((selected & 4U) != 0) {
                RandrNotifyEvent event;
                event.subtype = 1;
                event.timestamp = candidate.timestamp;
                event.config_timestamp = candidate.config_timestamp;
                event.window = subscription.window;
                event.crtc = candidate.current_mode == 0 ? 0 : randr_crtc_id;
                event.output = randr_output_id;
                event.mode = candidate.current_mode;
                event.rotation = candidate.rotation;
                events.emplace_back(subscription.client, std::move(event));
            }
            if ((selected & 8U) != 0) {
                RandrNotifyEvent event;
                event.subtype = 2;
                event.timestamp = current_time_;
                event.window = subscription.window;
                event.output = randr_output_id;
                event.atom = property;
                event.property_status = property_status;
                events.emplace_back(subscription.client, std::move(event));
            }
            if ((selected & 64U) != 0) {
                RandrNotifyEvent event;
                event.subtype = 5;
                event.timestamp = candidate.timestamp;
                event.window = subscription.window;
                events.emplace_back(subscription.client, std::move(event));
            }
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    return true;
}

RandrUpdate
ServerState::select_randr_input(std::uint32_t client,
                                std::uint32_t window_id,
                                std::uint16_t mask)
{
    RandrState candidate;
    try {
        candidate = randr_;
        auto &subscriptions = candidate.subscriptions;
        const auto existing = std::find_if(
            subscriptions.begin(), subscriptions.end(),
            [client, window_id](const RandrSubscription &subscription) {
                return subscription.client == client &&
                    subscription.window == window_id;
            });
        if (mask == 0) {
            if (existing != subscriptions.end())
                subscriptions.erase(existing);
        }
        else if (existing == subscriptions.end()) {
            subscriptions.push_back({client, window_id, mask});
        }
        else {
            existing->mask = mask;
        }
    }
    catch (const std::bad_alloc &) {
        return RandrUpdate::resource_exhausted;
    }

    std::vector<PlannedEvent> events;
    if ((mask & 1U) != 0) {
        try {
            events.emplace_back(
                client,
                RandrScreenChangeNotifyEvent{
                    candidate.timestamp, candidate.config_timestamp,
                    window_id, width_, height_,
                    wire_size(candidate.millimetre_width),
                    wire_size(candidate.millimetre_height),
                    candidate.rotation});
        }
        catch (const std::bad_alloc &) {
            return RandrUpdate::queue_full;
        }
    }
    if (!queue_events_atomically(events))
        return RandrUpdate::queue_full;
    randr_ = std::move(candidate);
    return RandrUpdate::updated;
}

RandrUpdate
ServerState::commit_randr_state(RandrState candidate,
                                std::uint16_t notify_mask,
                                AtomId property,
                                std::uint8_t property_status)
{
    try {
        update_automatic_randr_monitors(candidate, width_, height_);
    }
    catch (const std::bad_alloc &) {
        return RandrUpdate::resource_exhausted;
    }
    if ((notify_mask & ~8U) != 0)
        candidate.timestamp = current_time_;
    if ((notify_mask & (1U | 2U | 4U | 64U)) != 0)
        candidate.config_timestamp = current_time_;
    std::vector<PlannedEvent> events;
    if (!append_randr_events(candidate, width_, height_, notify_mask,
                             property, property_status, events) ||
        !queue_events_atomically(events)) {
        return RandrUpdate::queue_full;
    }
    randr_ = std::move(candidate);
    return RandrUpdate::updated;
}

RandrUpdate
ServerState::resize_randr_screen(RandrState candidate,
                                 std::uint16_t width,
                                 std::uint16_t height)
{
    auto *root = window(root_window_id);
    if (width == 0 || height == 0 || root == nullptr || !root->surface ||
        !composited_root_) {
        return RandrUpdate::invalid;
    }
    try {
        update_automatic_randr_monitors(candidate, width, height);
    }
    catch (const std::bad_alloc &) {
        return RandrUpdate::resource_exhausted;
    }
    auto root_surface = Surface::create(width, height, 24);
    auto composited_surface = Surface::create(width, height, 24);
    if (!root_surface || !composited_surface)
        return RandrUpdate::resource_exhausted;
    root_surface->copy_from(*root->surface, 0, 0, 0, 0, width, height, 3,
                            0xffffffffU);

    const std::size_t old_bytes = root->surface->storage_bytes() +
        composited_root_->storage_bytes();
    const std::size_t new_bytes = root_surface->storage_bytes() +
        composited_surface->storage_bytes();
    if (new_bytes > maximum_server_surface_bytes ||
        surface_budget_->bytes - old_bytes >
            maximum_server_surface_bytes - new_bytes) {
        return RandrUpdate::resource_exhausted;
    }

    const auto manage = [this](Surface surface)
        -> std::shared_ptr<Surface> {
        const std::size_t bytes = surface.storage_bytes();
        surface_budget_->bytes += bytes;
        try {
            auto allocation = std::make_shared<ManagedSurface>(
                std::move(surface), surface_budget_);
            return std::shared_ptr<Surface>(allocation,
                                            &allocation->surface);
        }
        catch (const std::bad_alloc &) {
            surface_budget_->bytes -= bytes;
            return {};
        }
    };
    auto managed_root = manage(std::move(*root_surface));
    if (!managed_root)
        return RandrUpdate::resource_exhausted;
    auto managed_composited = manage(std::move(*composited_surface));
    if (!managed_composited)
        return RandrUpdate::resource_exhausted;

    candidate.timestamp = current_time_;
    candidate.config_timestamp = current_time_;
    std::vector<PlannedEvent> events;
    constexpr std::uint16_t resize_notifications = 1U | 2U | 4U | 64U;
    if (!append_randr_events(candidate, width, height,
                             resize_notifications, 0, 0, events)) {
        return RandrUpdate::resource_exhausted;
    }
    if (!append_present_configure_events(
            root_window_id, 0, 0, width, height, events)) {
        return RandrUpdate::resource_exhausted;
    }
    if (!queue_events_atomically(events)) {
        return RandrUpdate::queue_full;
    }

    replace_window_surface(*root, std::move(managed_root));
    composited_root_ = std::move(managed_composited);
    root->width = width;
    root->height = height;
    width_ = width;
    height_ = height;
    input_.pointer_x = std::clamp<std::int32_t>(
        input_.pointer_x, 0, static_cast<std::int32_t>(width) - 1);
    input_.pointer_y = std::clamp<std::int32_t>(
        input_.pointer_y, 0, static_cast<std::int32_t>(height) - 1);
    randr_ = std::move(candidate);
    invalidate_scene();
    return RandrUpdate::updated;
}

bool
ServerState::broadcast_mapping_notify(std::uint8_t request,
                                      std::uint8_t first_keycode,
                                      std::uint8_t count)
{
    std::vector<PlannedEvent> events;
    try {
        events.reserve(clients_.size() + xkb_selections_.size());
        const MappingNotifyEvent core_event{
            request, first_keycode, count};
        for (const auto &client : clients_)
            events.emplace_back(client.first, core_event);

        if (request != 2) {
            XkbMapNotifyEvent xkb_event;
            xkb_event.time = current_time_;
            if (request == 1) {
                xkb_event.changed = (1U << 0) | (1U << 1);
                xkb_event.first_type = 0;
                xkb_event.type_count = 3;
                xkb_event.first_keysym = first_keycode;
                xkb_event.keysym_count = count;
            }
            else {
                xkb_event.changed = 1U << 2;
                xkb_event.first_modmap = minimum_keycode;
                xkb_event.modmap_count = static_cast<std::uint8_t>(
                    maximum_keycode - minimum_keycode + 1U);
            }
            for (const auto &selection : xkb_selections_) {
                if ((selection.events & (1U << 1)) != 0 &&
                    (selection.map & xkb_event.changed) != 0) {
                    events.emplace_back(selection.owner, xkb_event);
                }
            }
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    return queue_events_atomically(events);
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
    auto previous = std::move(candidate.shapes[kind]);
    candidate.shapes[kind] = std::move(shape);
    const auto prospective_cursor = current_cursor();
    shape = std::move(candidate.shapes[kind]);
    candidate.shapes[kind] = std::move(previous);
    if (append_cursor_change(prospective_cursor, events) ==
            EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return ShapeUpdate::queue_full;
    }
    candidate.shapes[kind] = std::move(shape);
    displayed_cursor_ = prospective_cursor;
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

RenderPicture *
ServerState::render_picture(std::uint32_t id)
{
    const auto found = render_pictures_.find(id);
    return found == render_pictures_.end() ? nullptr : found->second.get();
}

const RenderPicture *
ServerState::render_picture(std::uint32_t id) const
{
    const auto found = render_pictures_.find(id);
    return found == render_pictures_.end() ? nullptr : found->second.get();
}

std::shared_ptr<RenderPicture>
ServerState::render_picture_handle(std::uint32_t id) const
{
    const auto found = render_pictures_.find(id);
    return found == render_pictures_.end()
        ? std::shared_ptr<RenderPicture>{}
        : found->second;
}

bool
ServerState::add_render_picture(RenderPicture picture, std::uint32_t owner)
{
    const std::uint32_t id = picture.id;
    if (auto *drawable = std::get_if<RenderDrawableSource>(&picture.source)) {
        if (const auto *candidate = window(drawable->drawable)) {
            drawable->surface = candidate->surface;
            drawable->pixmap = false;
        }
        else if (const auto *candidate = pixmap(drawable->drawable)) {
            drawable->surface = candidate->surface;
            drawable->pixmap = true;
        }
        if (!drawable->surface)
            return false;
    }
    if (picture.attributes.alpha_map != 0 &&
        !picture.attributes.alpha_map_picture) {
        picture.attributes.alpha_map_picture =
            render_picture_handle(picture.attributes.alpha_map);
        if (!picture.attributes.alpha_map_picture)
            return false;
    }
    if (!resources_.insert(id, ResourceKind::render_picture, owner))
        return false;
    try {
        auto stored = std::make_shared<RenderPicture>(std::move(picture));
        if (!render_pictures_.emplace(id, std::move(stored)).second) {
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
ServerState::erase_render_picture(std::uint32_t id)
{
    if (render_pictures_.erase(id) == 0)
        return false;
    static_cast<void>(resources_.erase(id));
    return true;
}

RenderGlyphSet *
ServerState::render_glyph_set(std::uint32_t id)
{
    const auto found = render_glyph_sets_.find(id);
    return found == render_glyph_sets_.end() ? nullptr : &found->second;
}

const RenderGlyphSet *
ServerState::render_glyph_set(std::uint32_t id) const
{
    const auto found = render_glyph_sets_.find(id);
    return found == render_glyph_sets_.end() ? nullptr : &found->second;
}

bool
ServerState::add_render_glyph_set(RenderGlyphSet glyph_set,
                                  std::uint32_t owner)
{
    const std::uint32_t id = glyph_set.id;
    if (!glyph_set.storage ||
        !resources_.insert(id, ResourceKind::render_glyph_set, owner)) {
        return false;
    }
    try {
        if (!render_glyph_sets_.emplace(id, std::move(glyph_set)).second) {
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
ServerState::erase_render_glyph_set(std::uint32_t id)
{
    if (render_glyph_sets_.erase(id) == 0)
        return false;
    static_cast<void>(resources_.erase(id));
    return true;
}

bool
ServerState::render_glyph_storage_fits(
    const RenderGlyphStorage &changed,
    std::size_t changed_bytes) const noexcept
{
    std::size_t total = 0;
    for (const auto &entry : render_glyph_sets_) {
        if (!entry.second.storage)
            continue;
        const auto *storage = entry.second.storage.get();
        bool counted = false;
        for (const auto &other : render_glyph_sets_) {
            if (other.first < entry.first &&
                other.second.storage.get() == storage) {
                counted = true;
                break;
            }
        }
        if (counted)
            continue;
        const std::size_t bytes = storage == &changed
            ? changed_bytes
            : storage->bytes;
        if (bytes > maximum_server_render_glyph_bytes - total)
            return false;
        total += bytes;
    }
    return true;
}

CursorRecord *
ServerState::cursor(std::uint32_t id)
{
    const auto found = cursors_.find(id);
    return found == cursors_.end() ? nullptr : &found->second;
}

const CursorRecord *
ServerState::cursor(std::uint32_t id) const
{
    const auto found = cursors_.find(id);
    return found == cursors_.end() ? nullptr : &found->second;
}

bool
ServerState::add_cursor(CursorRecord cursor, std::uint32_t owner)
{
    const std::uint32_t id = cursor.id;
    if (!cursor.image || !resources_.insert(id, ResourceKind::cursor, owner))
        return false;
    if (cursor.image->serial == 0) {
        cursor.image->serial = next_cursor_serial_++;
        if (next_cursor_serial_ == 0)
            next_cursor_serial_ = 1;
    }
    try {
        if (!cursors_.emplace(id, std::move(cursor)).second) {
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
ServerState::erase_cursor(std::uint32_t id)
{
    if (cursors_.erase(id) == 0)
        return false;
    static_cast<void>(resources_.erase(id));
    return true;
}

EventDelivery
ServerState::append_cursor_change(
    const std::shared_ptr<CursorImage> &current,
    std::vector<PlannedEvent> &events) const
{
    if (current == displayed_cursor_)
        return EventDelivery::no_recipient;

    const std::size_t original_size = events.size();
    try {
        if (xfixes_cursor_inputs_.size() >
            events.max_size() - events.size()) {
            return EventDelivery::queue_full;
        }
        events.reserve(events.size() + xfixes_cursor_inputs_.size());
        for (const auto &selection : xfixes_cursor_inputs_) {
            if ((selection.event_mask & 1U) == 0)
                continue;
            events.emplace_back(
                selection.client,
                XFixesCursorNotifyEvent{
                    0, selection.window, current ? current->serial : 0,
                    current_time_, current ? current->name : 0});
        }
    }
    catch (const std::bad_alloc &) {
        return EventDelivery::queue_full;
    }
    return events.size() == original_size
        ? EventDelivery::no_recipient
        : EventDelivery::delivered;
}

EventDelivery
ServerState::cursor_maybe_changed()
{
    const auto current = current_cursor();
    std::vector<PlannedEvent> events;
    const EventDelivery delivery = append_cursor_change(current, events);
    if (delivery == EventDelivery::queue_full)
        return delivery;
    if (!queue_events_atomically(events))
        return EventDelivery::queue_full;
    displayed_cursor_ = current;
    return delivery;
}

XFixesUpdate
ServerState::set_window_cursor(
    WindowRecord &candidate, std::shared_ptr<CursorImage> cursor_image)
{
    const auto previous = candidate.cursor;
    candidate.cursor = cursor_image;
    const auto current = current_cursor();
    candidate.cursor = previous;

    std::vector<PlannedEvent> events;
    if (append_cursor_change(current, events) == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return XFixesUpdate::queue_full;
    }
    candidate.cursor = std::move(cursor_image);
    displayed_cursor_ = current;
    return XFixesUpdate::updated;
}

XFixesUpdate
ServerState::set_pointer_grab_cursor(
    std::uint32_t event_mask, std::shared_ptr<CursorImage> cursor_image)
{
    if (!input_.pointer_grab)
        return XFixesUpdate::invalid;
    ActiveGrab prospective = *input_.pointer_grab;
    prospective.event_mask = event_mask;
    prospective.cursor = cursor_image;
    const auto current = current_cursor_for(pointer_window(), &prospective);
    std::vector<PlannedEvent> events;
    if (append_cursor_change(current, events) == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return XFixesUpdate::queue_full;
    }
    input_.pointer_grab->event_mask = event_mask;
    input_.pointer_grab->cursor = std::move(cursor_image);
    displayed_cursor_ = current;
    return XFixesUpdate::updated;
}

XFixesUpdate
ServerState::replace_cursor(
    const std::shared_ptr<CursorImage> &source,
    const std::shared_ptr<CursorImage> &destination)
{
    if (!source || !destination)
        return XFixesUpdate::invalid;
    const auto previous = current_cursor();
    const auto current = previous == destination ? source : previous;
    std::vector<PlannedEvent> events;
    if (append_cursor_change(current, events) == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return XFixesUpdate::queue_full;
    }
    for (auto &entry : cursors_) {
        if (entry.second.image == destination)
            entry.second.image = source;
    }
    for (auto &entry : windows_) {
        if (entry.second.cursor == destination)
            entry.second.cursor = source;
    }
    for (auto &grab : passive_grabs_) {
        if (grab.cursor == destination)
            grab.cursor = source;
    }
    if (input_.pointer_grab && input_.pointer_grab->cursor == destination)
        input_.pointer_grab->cursor = source;
    if (input_.keyboard_grab && input_.keyboard_grab->cursor == destination)
        input_.keyboard_grab->cursor = source;
    displayed_cursor_ = current;
    return XFixesUpdate::updated;
}

XFixesUpdate
ServerState::replace_cursor_by_name(
    const std::shared_ptr<CursorImage> &source, AtomId name)
{
    if (!source || name == 0)
        return XFixesUpdate::invalid;
    const auto matches = [&](const std::shared_ptr<CursorImage> &image) {
        return image && image != source && image->name == name;
    };
    const auto previous = current_cursor();
    const auto current = matches(previous) ? source : previous;
    std::vector<PlannedEvent> events;
    if (append_cursor_change(current, events) == EventDelivery::queue_full ||
        !queue_events_atomically(events)) {
        return XFixesUpdate::queue_full;
    }
    for (auto &entry : cursors_) {
        if (matches(entry.second.image))
            entry.second.image = source;
    }
    for (auto &entry : windows_) {
        if (matches(entry.second.cursor))
            entry.second.cursor = source;
    }
    for (auto &grab : passive_grabs_) {
        if (matches(grab.cursor))
            grab.cursor = source;
    }
    if (input_.pointer_grab && matches(input_.pointer_grab->cursor))
        input_.pointer_grab->cursor = source;
    if (input_.keyboard_grab && matches(input_.keyboard_grab->cursor))
        input_.keyboard_grab->cursor = source;
    displayed_cursor_ = current;
    return XFixesUpdate::updated;
}

Region *
ServerState::xfixes_region(std::uint32_t id)
{
    const auto found = xfixes_regions_.find(id);
    return found == xfixes_regions_.end() ? nullptr : &found->second;
}

const Region *
ServerState::xfixes_region(std::uint32_t id) const
{
    const auto found = xfixes_regions_.find(id);
    return found == xfixes_regions_.end() ? nullptr : &found->second;
}

bool
ServerState::add_xfixes_region(std::uint32_t id, Region region,
                               std::uint32_t owner)
{
    if (xfixes_regions_.size() >= maximum_xfixes_regions ||
        !resources_.insert(id, ResourceKind::xfixes_region, owner)) {
        return false;
    }
    try {
        if (!xfixes_regions_.emplace(id, std::move(region)).second) {
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
ServerState::erase_xfixes_region(std::uint32_t id)
{
    if (xfixes_regions_.erase(id) == 0)
        return false;
    static_cast<void>(resources_.erase(id));
    return true;
}

XFixesUpdate
ServerState::select_xfixes_selection_input(
    std::uint32_t client, std::uint32_t window_id, AtomId selection,
    std::uint32_t event_mask)
{
    const auto matches = [=](const XFixesSelectionSubscription &entry) {
        return entry.client == client && entry.window == window_id &&
            entry.selection == selection;
    };
    const auto found = std::find_if(
        xfixes_selection_inputs_.begin(), xfixes_selection_inputs_.end(),
        matches);
    if (event_mask == 0) {
        if (found != xfixes_selection_inputs_.end())
            xfixes_selection_inputs_.erase(found);
        return XFixesUpdate::updated;
    }
    if (found != xfixes_selection_inputs_.end()) {
        found->event_mask = event_mask;
        return XFixesUpdate::updated;
    }
    if (xfixes_selection_inputs_.size() >= maximum_xfixes_subscriptions)
        return XFixesUpdate::resource_exhausted;
    try {
        xfixes_selection_inputs_.push_back(
            {client, window_id, selection, event_mask});
    }
    catch (const std::bad_alloc &) {
        return XFixesUpdate::resource_exhausted;
    }
    return XFixesUpdate::updated;
}

XFixesUpdate
ServerState::select_xfixes_cursor_input(
    std::uint32_t client, std::uint32_t window_id,
    std::uint32_t event_mask)
{
    const auto matches = [=](const XFixesCursorSubscription &entry) {
        return entry.client == client && entry.window == window_id;
    };
    const auto found = std::find_if(
        xfixes_cursor_inputs_.begin(), xfixes_cursor_inputs_.end(), matches);
    if (event_mask == 0) {
        if (found != xfixes_cursor_inputs_.end())
            xfixes_cursor_inputs_.erase(found);
        return XFixesUpdate::updated;
    }
    if (found != xfixes_cursor_inputs_.end()) {
        found->event_mask = event_mask;
        return XFixesUpdate::updated;
    }
    if (xfixes_cursor_inputs_.size() >= maximum_xfixes_subscriptions)
        return XFixesUpdate::resource_exhausted;
    try {
        xfixes_cursor_inputs_.push_back({client, window_id, event_mask});
    }
    catch (const std::bad_alloc &) {
        return XFixesUpdate::resource_exhausted;
    }
    return XFixesUpdate::updated;
}

XFixesUpdate
ServerState::hide_cursor(std::uint32_t client)
{
    try {
        auto &count = cursor_hide_counts_[client];
        if (count != std::numeric_limits<std::uint32_t>::max())
            ++count;
    }
    catch (const std::bad_alloc &) {
        return XFixesUpdate::resource_exhausted;
    }
    return XFixesUpdate::updated;
}

XFixesUpdate
ServerState::show_cursor(std::uint32_t client)
{
    const auto found = cursor_hide_counts_.find(client);
    if (found == cursor_hide_counts_.end())
        return XFixesUpdate::invalid;
    if (--found->second == 0)
        cursor_hide_counts_.erase(found);
    return XFixesUpdate::updated;
}

bool
ServerState::add_xfixes_barrier(XFixesBarrierRecord barrier,
                                std::uint32_t owner)
{
    const std::uint32_t id = barrier.id;
    if (xfixes_barriers_.size() >= maximum_xfixes_barriers ||
        !resources_.insert(id, ResourceKind::xfixes_barrier, owner)) {
        return false;
    }
    try {
        if (!xfixes_barriers_.emplace(id, std::move(barrier)).second) {
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

const XFixesBarrierRecord *
ServerState::xfixes_barrier(std::uint32_t id) const noexcept
{
    const auto found = xfixes_barriers_.find(id);
    return found == xfixes_barriers_.end() ? nullptr : &found->second;
}

bool
ServerState::erase_xfixes_barrier(std::uint32_t id, std::uint32_t owner)
{
    const auto record = resources_.find(id);
    if (!record || record->kind != ResourceKind::xfixes_barrier ||
        record->owner != owner || xfixes_barriers_.erase(id) == 0) {
        return false;
    }
    static_cast<void>(resources_.erase(id));
    return true;
}

XFixesUpdate
ServerState::alter_save_set(std::uint32_t client, std::uint32_t window_id,
                            bool insert, bool to_root, bool map)
{
    auto found = save_sets_.find(client);
    if (!insert) {
        if (found == save_sets_.end())
            return XFixesUpdate::updated;
        auto &entries = found->second;
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                           [window_id](const SaveSetEntry &entry) {
                               return entry.window == window_id;
                           }),
            entries.end());
        if (entries.empty())
            save_sets_.erase(found);
        return XFixesUpdate::updated;
    }
    try {
        auto &entries = save_sets_[client];
        if (std::none_of(
                entries.begin(), entries.end(),
                [window_id](const SaveSetEntry &entry) {
                    return entry.window == window_id;
                })) {
            entries.push_back({window_id, to_root, map});
        }
    }
    catch (const std::bad_alloc &) {
        return XFixesUpdate::resource_exhausted;
    }
    return XFixesUpdate::updated;
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
    const bool inserted = found == selections_.end();
    if (found == selections_.end()) {
        try {
            found = selections_.emplace(selection, SelectionRecord{}).first;
        }
        catch (const std::bad_alloc &) {
            return SelectionUpdate::event_queue_full;
        }
    }
    const SelectionRecord previous = found->second;
    std::vector<PlannedEvent> events;
    try {
        events.reserve(xfixes_selection_inputs_.size() + 1);
        if (clear_previous) {
            events.emplace_back(
                previous.client,
                SelectionClearEvent{
                    effective_time, previous.window, selection});
        }
        for (const auto &subscription : xfixes_selection_inputs_) {
            if (subscription.selection != selection ||
                (subscription.event_mask & 1U) == 0) {
                continue;
            }
            events.emplace_back(
                subscription.client,
                XFixesSelectionNotifyEvent{
                    0, subscription.window, window_id, selection,
                    current_time_, effective_time});
        }
    }
    catch (const std::bad_alloc &) {
        if (inserted)
            selections_.erase(found);
        return SelectionUpdate::event_queue_full;
    }
    if (!queue_events_atomically(events)) {
        if (inserted)
            selections_.erase(found);
        return SelectionUpdate::event_queue_full;
    }
    found->second = SelectionRecord{
        window_id, window_id == 0 ? 0 : client, effective_time};
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
        if (grab == nullptr || grab->xi2 ||
            (grab->event_mask & mask) == 0)
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
            const auto xkb = xkb_state();
            std::uint32_t buttons = 0;
            for (std::uint32_t button = 1; button <= 5; ++button) {
                if ((state & (1U << (button + 7))) != 0)
                    buttons |= 1U << button;
            }
            for (const auto &selection : xi2_selections_) {
                bool selected = selection.window == destination &&
                    xi2_mask_selected(
                        selection, xi2_pointer_device_id, type);
                if (grab != nullptr) {
                    if (selection.owner != grab->owner)
                        continue;
                    if (grab->xi2) {
                        selected = (grab->owner_events && selected) ||
                            (destination == grab->window &&
                             (grab->xi2_event_mask & (1U << type)) != 0);
                    }
                    else {
                        selected = grab->owner_events && selected;
                    }
                }
                if (!selected) {
                    continue;
                }
                events.emplace_back(
                    selection.owner,
                    Xi2CrossingEvent{
                        type, xi2_pointer_device_id,
                        xi2_pointer_device_id, current_time_, mode, detail,
                        root_window_id, destination, child,
                        root_x, root_y, root_x - origin.first,
                        root_y - origin.second, buttons,
                        xkb.base_mods, xkb.latched_mods, xkb.locked_mods,
                        xkb.mods, static_cast<std::uint8_t>(xkb.base_group),
                        static_cast<std::uint8_t>(xkb.latched_group),
                        xkb.locked_group, xkb.group, true, event.focus});
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
            const auto origin = absolute_position(destination);
            std::uint32_t child = pointer_window;
            while (child != 0 && child != destination) {
                const auto *candidate = window(child);
                if (candidate == nullptr)
                    break;
                if (candidate->parent == destination)
                    break;
                child = candidate->parent;
            }
            if (child == destination)
                child = 0;
            const auto xkb = xkb_state();
            std::uint32_t buttons = 0;
            for (std::uint32_t button = 1; button <= 5; ++button) {
                if (input_.pressed_buttons.test(button))
                    buttons |= 1U << button;
            }
            for (const auto &selection : xi2_selections_) {
                if (selection.window != destination ||
                    !xi2_mask_selected(
                        selection, xi2_keyboard_device_id, type)) {
                    continue;
                }
                events.emplace_back(
                    selection.owner,
                    Xi2CrossingEvent{
                        type, xi2_keyboard_device_id,
                        xi2_keyboard_device_id, current_time_, mode, detail,
                        root_window_id, destination, child,
                        input_.pointer_x, input_.pointer_y,
                        input_.pointer_x - origin.first,
                        input_.pointer_y - origin.second, buttons,
                        xkb.base_mods, xkb.latched_mods, xkb.locked_mods,
                        xkb.mods, static_cast<std::uint8_t>(xkb.base_group),
                        static_cast<std::uint8_t>(xkb.latched_group),
                        xkb.locked_group, xkb.group, true, false});
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
    std::uint16_t state = static_cast<std::uint16_t>(
        xkb_base_modifiers(input_.pressed_keys) |
        input_.xkb.latched_mods | input_.xkb.locked_mods);
    state &= 0x00ffU;
    for (std::size_t button = 1; button <= 5; ++button) {
        if (input_.pressed_buttons.test(button))
            state |= static_cast<std::uint16_t>(1U << (button + 7));
    }
    input_.modifier_button_mask = state;
}

std::uint8_t
ServerState::xkb_base_modifiers(
    const std::array<std::uint8_t, 32> &keys) const noexcept
{
    std::uint8_t state = 0;
    for (std::size_t group = 0; group < 8; ++group) {
        for (std::size_t index = 0;
             index < input_.modifier_keys_per_group; ++index) {
            const std::uint8_t keycode = input_.modifier_map[
                group * input_.modifier_keys_per_group + index];
            if (keycode != 0 &&
                (keys[keycode >> 3] &
                 (1U << (keycode & 7U))) != 0) {
                state |= static_cast<std::uint8_t>(1U << group);
                break;
            }
        }
    }
    return state;
}

XkbStateSnapshot
ServerState::xkb_state_for(
    const std::array<std::uint8_t, 32> &keys,
    const XkbKeyboardState &state) const noexcept
{
    XkbStateSnapshot snapshot;
    snapshot.base_mods = xkb_base_modifiers(keys);
    snapshot.latched_mods = state.latched_mods;
    snapshot.locked_mods = state.locked_mods;
    snapshot.mods = static_cast<std::uint8_t>(
        snapshot.base_mods | snapshot.latched_mods |
        snapshot.locked_mods);
    snapshot.base_group = state.base_group;
    snapshot.latched_group = state.latched_group;
    snapshot.locked_group = state.locked_group;
    // The product has one group, so every legal base/latch/lock combination
    // resolves to group zero while the component values remain queryable.
    snapshot.group = 0;
    snapshot.pointer_buttons = static_cast<std::uint16_t>(
        input_.modifier_button_mask & 0x1f00U);
    return snapshot;
}

XkbStateSnapshot
ServerState::xkb_state() const noexcept
{
    return xkb_state_for(input_.pressed_keys, input_.xkb);
}

std::uint32_t
ServerState::xkb_indicator_state() const noexcept
{
    std::uint32_t result = input_.led_mask;
    if ((input_.xkb.locked_mods & (1U << 1)) != 0)
        result |= 1U << 0; // Caps Lock
    if ((input_.xkb.locked_mods & (1U << 4)) != 0)
        result |= 1U << 1; // Num Lock
    if ((input_.xkb.locked_mods & (1U << 5)) != 0)
        result |= 1U << 2; // Scroll Lock
    return result;
}

const XkbEventSelection *
ServerState::xkb_selection(std::uint32_t owner) const noexcept
{
    const auto found = std::find_if(
        xkb_selections_.begin(), xkb_selections_.end(),
        [owner](const XkbEventSelection &selection) {
            return selection.owner == owner;
        });
    return found == xkb_selections_.end() ? nullptr : &*found;
}

XkbUpdate
ServerState::select_xkb_events(XkbEventSelection selection)
{
    const auto found = std::find_if(
        xkb_selections_.begin(), xkb_selections_.end(),
        [&selection](const XkbEventSelection &existing) {
            return existing.owner == selection.owner;
        });
    if (selection.events == 0) {
        if (found != xkb_selections_.end())
            xkb_selections_.erase(found);
        return XkbUpdate::updated;
    }
    if (found != xkb_selections_.end()) {
        *found = selection;
        return XkbUpdate::updated;
    }
    if (xkb_selections_.size() >= maximum_pending_events)
        return XkbUpdate::resource_exhausted;
    try {
        xkb_selections_.push_back(selection);
    }
    catch (const std::bad_alloc &) {
        return XkbUpdate::resource_exhausted;
    }
    return XkbUpdate::updated;
}

bool
ServerState::append_xkb_state_events(
    const XkbStateSnapshot &before, const XkbStateSnapshot &after,
    std::uint8_t keycode, std::uint8_t event_type,
    std::uint8_t request_major, std::uint8_t request_minor,
    std::vector<PlannedEvent> &events) const
{
    std::uint16_t changed = 0;
    if (before.mods != after.mods)
        changed |= 1U << 0;
    if (before.base_mods != after.base_mods)
        changed |= 1U << 1;
    if (before.latched_mods != after.latched_mods)
        changed |= 1U << 2;
    if (before.locked_mods != after.locked_mods)
        changed |= 1U << 3;
    if (before.group != after.group)
        changed |= 1U << 4;
    if (before.base_group != after.base_group)
        changed |= 1U << 5;
    if (before.latched_group != after.latched_group)
        changed |= 1U << 6;
    if (before.locked_group != after.locked_group)
        changed |= 1U << 7;
    if (before.pointer_buttons != after.pointer_buttons)
        changed |= 1U << 13;
    if (changed == 0)
        return true;
    try {
        for (const auto &selection : xkb_selections_) {
            if ((selection.events & (1U << 2)) == 0 ||
                (selection.state & changed) == 0) {
                continue;
            }
            events.emplace_back(
                selection.owner,
                XkbStateNotifyEvent{
                    current_time_, xkb_keyboard_device_id,
                    after.mods, after.base_mods, after.latched_mods,
                    after.locked_mods, after.group, after.base_group,
                    after.latched_group, after.locked_group,
                    after.pointer_buttons, changed, keycode, event_type,
                    request_major, request_minor});
        }
    }
    catch (const std::bad_alloc &) {
        return false;
    }
    return true;
}

XkbUpdate
ServerState::latch_lock_xkb(
    std::uint8_t affect_locks, std::uint8_t locks,
    bool lock_group, std::uint8_t group_lock,
    std::uint8_t affect_latches, bool latch_group,
    std::int16_t group_latch,
    std::uint8_t request_major, std::uint8_t request_minor)
{
    XkbKeyboardState candidate = input_.xkb;
    candidate.locked_mods = static_cast<std::uint8_t>(
        (candidate.locked_mods & ~affect_locks) |
        (locks & affect_locks));
    candidate.latched_mods = static_cast<std::uint8_t>(
        candidate.latched_mods & ~affect_latches);
    if (lock_group)
        candidate.locked_group = static_cast<std::uint8_t>(group_lock & 3U);
    if (latch_group)
        candidate.latched_group = group_latch;

    const auto before = xkb_state();
    const auto after = xkb_state_for(input_.pressed_keys, candidate);
    std::vector<PlannedEvent> events;
    if (!append_xkb_state_events(
            before, after, 0, 0, request_major, request_minor, events)) {
        return XkbUpdate::resource_exhausted;
    }
    if (!queue_events_atomically(events))
        return XkbUpdate::queue_full;
    input_.xkb = candidate;
    refresh_modifier_button_mask();
    return XkbUpdate::updated;
}

XkbUpdate
ServerState::set_xkb_controls(
    XkbControls controls, std::uint32_t changed,
    std::uint32_t enabled_changes,
    std::uint8_t request_major, std::uint8_t request_minor)
{
    std::vector<PlannedEvent> events;
    try {
        for (const auto &selection : xkb_selections_) {
            if ((selection.events & (1U << 3)) == 0 ||
                (selection.controls & changed) == 0) {
                continue;
            }
            events.emplace_back(
                selection.owner,
                XkbControlsNotifyEvent{
                    current_time_, xkb_keyboard_device_id, 1, changed,
                    controls.enabled, enabled_changes, 0, 0,
                    request_major, request_minor});
        }
    }
    catch (const std::bad_alloc &) {
        return XkbUpdate::resource_exhausted;
    }
    if (!queue_events_atomically(events))
        return XkbUpdate::queue_full;
    input_.xkb.controls = controls;
    input_.auto_repeats = controls.per_key_repeat;
    input_.global_auto_repeat = (controls.enabled & 1U) != 0;
    update_repeat_controls();
    return XkbUpdate::updated;
}

std::uint32_t
ServerState::xkb_client_flags(std::uint32_t owner) const noexcept
{
    const auto found = xkb_client_flags_.find(owner);
    return found == xkb_client_flags_.end() ? 0 : found->second;
}

XkbUpdate
ServerState::set_xkb_client_flags(std::uint32_t owner, std::uint32_t value)
{
    try {
        if (value == 0)
            xkb_client_flags_.erase(owner);
        else
            xkb_client_flags_[owner] = value;
    }
    catch (const std::bad_alloc &) {
        return XkbUpdate::resource_exhausted;
    }
    return XkbUpdate::updated;
}

const Xi2EventSelection *
ServerState::xi2_selection(std::uint32_t owner,
                           std::uint32_t window_id) const noexcept
{
    const auto found = std::find_if(
        xi2_selections_.begin(), xi2_selections_.end(),
        [owner, window_id](const Xi2EventSelection &selection) {
            return selection.owner == owner &&
                selection.window == window_id;
        });
    return found == xi2_selections_.end() ? nullptr : &*found;
}

Xi2Update
ServerState::select_xi2_events(Xi2EventSelection selection)
{
    const auto found = std::find_if(
        xi2_selections_.begin(), xi2_selections_.end(),
        [&selection](const Xi2EventSelection &existing) {
            return existing.owner == selection.owner &&
                existing.window == selection.window;
        });
    selection.masks.erase(
        std::remove_if(
            selection.masks.begin(), selection.masks.end(),
            [](const Xi2EventMask &mask) {
                return std::none_of(
                    mask.words.begin(), mask.words.end(),
                    [](std::uint32_t word) { return word != 0; });
            }),
        selection.masks.end());
    if (selection.masks.empty()) {
        if (found != xi2_selections_.end())
            xi2_selections_.erase(found);
        return Xi2Update::updated;
    }
    if (found != xi2_selections_.end()) {
        *found = std::move(selection);
        return Xi2Update::updated;
    }
    if (xi2_selections_.size() >= maximum_xi2_selections)
        return Xi2Update::resource_exhausted;
    try {
        xi2_selections_.push_back(std::move(selection));
    }
    catch (const std::bad_alloc &) {
        return Xi2Update::resource_exhausted;
    }
    return Xi2Update::updated;
}

const std::unordered_map<AtomId, PropertyValue> &
ServerState::xi2_properties(std::uint16_t device) const noexcept
{
    static const std::unordered_map<AtomId, PropertyValue> empty;
    if (device < xi2_pointer_device_id ||
        device > xi2_keyboard_device_id) {
        return empty;
    }
    return xi2_properties_[device - xi2_pointer_device_id];
}

bool
ServerState::xi2_mask_selected(const Xi2EventSelection &selection,
                               std::uint16_t device,
                               std::uint16_t event_type) const noexcept
{
    const std::size_t word = event_type / 32;
    const std::uint32_t bit = 1U << (event_type % 32);
    return std::any_of(
        selection.masks.begin(), selection.masks.end(),
        [device, word, bit](const Xi2EventMask &mask) {
            const bool matches = mask.device == xi2_all_devices ||
                mask.device == xi2_all_master_devices ||
                mask.device == device;
            return matches && word < mask.words.size() &&
                (mask.words[word] & bit) != 0;
        });
}

Xi2Update
ServerState::set_xi2_property(std::uint16_t device, AtomId property,
                              PropertyValue value)
{
    auto &properties = xi2_properties_[device - xi2_pointer_device_id];
    const auto found = properties.find(property);
    if (found == properties.end() &&
        properties.size() >= maximum_xi2_properties_per_device) {
        return Xi2Update::resource_exhausted;
    }
    const std::size_t old_size = found == properties.end()
        ? 0
        : found->second.data.size();
    if (value.data.size() > maximum_property_bytes ||
        property_bytes_ - old_size >
            maximum_server_property_bytes - value.data.size()) {
        return Xi2Update::resource_exhausted;
    }
    std::unordered_map<AtomId, PropertyValue> candidate;
    const std::size_t new_size = value.data.size();
    try {
        candidate = properties;
        candidate.insert_or_assign(property, std::move(value));
    }
    catch (const std::bad_alloc &) {
        return Xi2Update::resource_exhausted;
    }
    std::vector<PlannedEvent> events;
    try {
        for (const auto &selection : xi2_selections_) {
            if (!xi2_mask_selected(selection, device, 12))
                continue;
            const bool already = std::any_of(
                events.begin(), events.end(),
                [&selection](const PlannedEvent &planned) {
                    return planned.first == selection.owner;
                });
            if (!already) {
                events.emplace_back(
                    selection.owner,
                    Xi2PropertyEvent{device, current_time_, property,
                        found == properties.end() ? std::uint8_t{1}
                                                  : std::uint8_t{2}});
            }
        }
    }
    catch (const std::bad_alloc &) {
        return Xi2Update::resource_exhausted;
    }
    if (!queue_events_atomically(events))
        return Xi2Update::queue_full;
    properties.swap(candidate);
    property_bytes_ = property_bytes_ - old_size + new_size;
    return Xi2Update::updated;
}

Xi2Update
ServerState::delete_xi2_property(std::uint16_t device, AtomId property)
{
    auto &properties = xi2_properties_[device - xi2_pointer_device_id];
    const auto found = properties.find(property);
    if (found == properties.end())
        return Xi2Update::updated;
    std::vector<PlannedEvent> events;
    try {
        for (const auto &selection : xi2_selections_) {
            if (!xi2_mask_selected(selection, device, 12))
                continue;
            const bool already = std::any_of(
                events.begin(), events.end(),
                [&selection](const PlannedEvent &planned) {
                    return planned.first == selection.owner;
                });
            if (!already) {
                events.emplace_back(
                    selection.owner,
                    Xi2PropertyEvent{device, current_time_, property, 0});
            }
        }
    }
    catch (const std::bad_alloc &) {
        return Xi2Update::resource_exhausted;
    }
    if (!queue_events_atomically(events))
        return Xi2Update::queue_full;
    property_bytes_ -= found->second.data.size();
    properties.erase(found);
    return Xi2Update::updated;
}

bool
ServerState::append_xi2_input_events(
    std::uint8_t type, std::uint8_t detail, std::uint8_t raw_detail,
    std::uint32_t source_window,
    std::int32_t root_x, std::int32_t root_y, std::uint16_t state,
    const XkbStateSnapshot &xkb, std::uint32_t flags,
    const ActiveGrab *grab,
    std::vector<PlannedEvent> &events) const
{
    const std::uint16_t device = type == 2 || type == 3
        ? xi2_keyboard_device_id
        : xi2_pointer_device_id;
    const std::uint16_t raw_type = static_cast<std::uint16_t>(type + 11);
    const std::size_t initial_size = events.size();
    try {
        for (const auto &selection : xi2_selections_) {
            if (selection.window != root_window_id ||
                !xi2_mask_selected(selection, device, raw_type)) {
                continue;
            }
            const bool already = std::any_of(
                events.begin() + static_cast<std::ptrdiff_t>(initial_size),
                events.end(), [&selection](const PlannedEvent &planned) {
                    return planned.first == selection.owner &&
                        std::holds_alternative<Xi2RawEvent>(planned.second);
                });
            if (!already) {
                events.emplace_back(
                    selection.owner,
                    Xi2RawEvent{raw_type, device, device, current_time_,
                                raw_detail, root_x, root_y, flags});
            }
        }
        if (source_window == 0)
            return true;
        const auto selected_on_path = [this, source_window, device, type](
            const Xi2EventSelection &selection) {
            return xi2_mask_selected(selection, device, type) &&
                (selection.window == source_window ||
                 is_descendant(source_window, selection.window));
        };
        const auto closer_selection = [&](
            const Xi2EventSelection &selection) {
            return std::any_of(
                xi2_selections_.begin(), xi2_selections_.end(),
                [this, &selection, source_window, device, type](
                    const Xi2EventSelection &candidate) {
                    return candidate.owner == selection.owner &&
                        candidate.window != selection.window &&
                        xi2_mask_selected(candidate, device, type) &&
                        (candidate.window == source_window ||
                         is_descendant(source_window, candidate.window)) &&
                        is_descendant(candidate.window, selection.window);
                });
        };
        const auto append_normal = [&](std::uint32_t owner,
                                       std::uint32_t destination) {
            const auto origin = absolute_position(destination);
            std::uint32_t child = source_window;
            while (child != 0 && child != destination) {
                const auto *candidate = window(child);
                if (candidate == nullptr)
                    break;
                if (candidate->parent == destination)
                    break;
                child = candidate->parent;
            }
            if (child == destination)
                child = 0;
            std::uint32_t buttons = 0;
            for (std::uint32_t button = 1; button <= 5; ++button) {
                if ((state & (1U << (button + 7))) != 0)
                    buttons |= 1U << button;
            }
            events.emplace_back(
                owner,
                Xi2DeviceEvent{
                    type, device, device, current_time_, detail,
                    root_window_id, destination, child,
                    root_x, root_y, root_x - origin.first,
                    root_y - origin.second, buttons,
                    xkb.base_mods, xkb.latched_mods, xkb.locked_mods,
                    xkb.mods, static_cast<std::uint8_t>(xkb.base_group),
                    static_cast<std::uint8_t>(xkb.latched_group),
                    xkb.locked_group, xkb.group, flags});
        };
        if (grab != nullptr) {
            bool normally_delivered = false;
            if (grab->owner_events) {
                for (const auto &selection : xi2_selections_) {
                    if (selection.owner != grab->owner ||
                        !selected_on_path(selection) ||
                        closer_selection(selection)) {
                        continue;
                    }
                    append_normal(selection.owner, selection.window);
                    normally_delivered = true;
                }
            }
            if (grab->xi2 && !normally_delivered &&
                (grab->xi2_event_mask & (1U << type)) != 0) {
                append_normal(grab->owner, grab->window);
            }
            return true;
        }
        for (const auto &selection : xi2_selections_) {
            if (!selected_on_path(selection) ||
                closer_selection(selection)) {
                continue;
            }
            append_normal(selection.owner, selection.window);
        }
    }
    catch (const std::bad_alloc &) {
        events.resize(initial_size);
        return false;
    }
    return true;
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
        const std::size_t route_start = events.size();
        const EventDelivery routed = source == 0
            ? EventDelivery::no_recipient
            : route_input_event(
                event, repeated.first == 2 ? 1U << 0 : 1U << 1,
                source, propagation_stop, pointer_window, grab, events);
        if (routed == EventDelivery::queue_full)
            return routed;
        if (repeated.first == 3) {
            events.erase(
                std::remove_if(
                    events.begin() +
                        static_cast<std::ptrdiff_t>(route_start),
                    events.end(),
                    [this](const PlannedEvent &planned) {
                        return (xkb_client_flags(planned.first) & 1U) != 0;
                    }),
                events.end());
        }
        delivered = delivered || routed == EventDelivery::delivered;
        const std::size_t xi2_start = events.size();
        if (!append_xi2_input_events(
                repeated.first, detail, detail, source,
                input_.pointer_x, input_.pointer_y, repeated.second,
                xkb_state(), repeated.first == 2 ? (1U << 16) : 0,
                grab,
                events)) {
            return EventDelivery::queue_full;
        }
        delivered = delivered || events.size() != xi2_start;
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
    const auto now = clock_.now();
    int timeout = -1;
    const auto consider = [&timeout](Clock::time_point::duration remaining) {
        if (remaining <= Clock::time_point::duration::zero()) {
            timeout = 0;
            return;
        }
        auto milliseconds = std::chrono::duration_cast<
            std::chrono::milliseconds>(remaining);
        if (milliseconds < remaining)
            milliseconds += std::chrono::milliseconds{1};
        const int candidate = milliseconds.count() >
                std::numeric_limits<int>::max()
            ? std::numeric_limits<int>::max()
            : static_cast<int>(milliseconds.count());
        timeout = timeout < 0 ? candidate : std::min(timeout, candidate);
    };
    if (key_repeat_)
        consider(key_repeat_->deadline - now);

    const std::uint64_t current = present_msc();
    const auto elapsed = now - present_epoch_;
    const auto elapsed_microseconds = elapsed <=
            Clock::time_point::duration::zero()
        ? std::uint64_t{0}
        : static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(
                  elapsed).count());
    const auto interval = static_cast<std::uint64_t>(
        present_refresh_interval.count());
    const std::uint64_t phase = elapsed_microseconds % interval;
    constexpr std::uint64_t maximum_timeout_microseconds =
        static_cast<std::uint64_t>(std::numeric_limits<int>::max()) * 1000U;
    for (const auto &operation : present_operations_) {
        if (!present_wait_ready(operation))
            continue;
        if (operation.target_msc <= current) {
            timeout = 0;
            break;
        }
        const std::uint64_t frames = operation.target_msc - current;
        if (frames >
            (maximum_timeout_microseconds + phase) / interval) {
            timeout = timeout < 0
                ? std::numeric_limits<int>::max()
                : timeout;
            continue;
        }
        const std::uint64_t remaining_microseconds =
            frames * interval - phase;
        const auto remaining = std::chrono::microseconds{
            static_cast<std::chrono::microseconds::rep>(
                remaining_microseconds)};
        consider(remaining);
    }
    return timeout;
}

EventDelivery
ServerState::process_timers()
{
    update_repeat_controls();
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
            key_repeat_->deadline = now + std::chrono::milliseconds{
                input_.xkb.controls.repeat_interval};
            return result;
        }
        delivered = delivered || result == EventDelivery::delivered;
        key_repeat_->deadline += std::chrono::milliseconds{
            input_.xkb.controls.repeat_interval};
        ++repeated;
    }
    if (key_repeat_ && key_repeat_->deadline <= now)
        key_repeat_->deadline = now + std::chrono::milliseconds{
            input_.xkb.controls.repeat_interval};

    const std::uint64_t msc = present_msc();
    const std::uint64_t ust = present_ust();
    constexpr std::size_t maximum_present_burst = 64;
    std::size_t completed = 0;
    for (auto operation = present_operations_.begin();
         operation != present_operations_.end() &&
         completed < maximum_present_burst;) {
        if (operation->target_msc > msc || !present_wait_ready(*operation)) {
            ++operation;
            continue;
        }
        const PresentUpdate result = execute_present(*operation, msc, ust);
        if (result == PresentUpdate::queue_full ||
            result == PresentUpdate::resource_exhausted) {
            operation->target_msc = msc ==
                    std::numeric_limits<std::uint64_t>::max()
                ? msc
                : msc + 1;
            return EventDelivery::queue_full;
        }
        operation = present_operations_.erase(operation);
        delivered = true;
        ++completed;
    }
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
    auto prospective_keys = input_.pressed_keys;
    auto prospective_buttons = input_.pressed_buttons;
    XkbKeyboardState prospective_xkb = input_.xkb;
    const XkbStateSnapshot xkb_before = xkb_state();
    if (key_event) {
        const std::uint8_t bit = static_cast<std::uint8_t>(
            1U << (detail & 7U));
        auto &keys = prospective_keys[detail >> 3];
        if (type == 2) {
            keys |= bit;
            // The fixed PC105 map has two locking actions needed by normal
            // desktop clients.  They update the same state exposed by XKB.
            if (detail == 66)
                prospective_xkb.locked_mods ^= 1U << 1; // Caps Lock
            else if (detail == 77)
                prospective_xkb.locked_mods ^= 1U << 4; // Num Lock
        }
        else {
            keys &= static_cast<std::uint8_t>(~bit);
        }
    }
    else if (button_event) {
        prospective_buttons.set(detail, type == 4);
    }
    if (motion_event) {
        constrain_pointer_by_barriers(
            input_.pointer_x, input_.pointer_y, root_x, root_y);
    }
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
                grab_detail, false, passive.cursor,
                passive.xi2, passive.xi2_event_mask};
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
                false, 0, true, {}};
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
    std::optional<ActiveGrab> prospective_pointer_grab = input_.pointer_grab;
    if (activated && button_event)
        prospective_pointer_grab = *activated;
    if (automatic)
        prospective_pointer_grab = *automatic;
    if (releases_pointer_grab)
        prospective_pointer_grab.reset();
    const auto prospective_cursor = current_cursor_for(
        pointer_window,
        prospective_pointer_grab ? &*prospective_pointer_grab : nullptr);
    const EventDelivery cursor = append_cursor_change(
        prospective_cursor, events);
    if (cursor == EventDelivery::queue_full)
        return cursor;
    XkbStateSnapshot xkb_after = xkb_state_for(
        prospective_keys, prospective_xkb);
    xkb_after.pointer_buttons = 0;
    for (std::size_t button = 1; button <= 5; ++button) {
        if (prospective_buttons.test(button)) {
            xkb_after.pointer_buttons |= static_cast<std::uint16_t>(
                1U << (button + 7));
        }
    }
    if (!append_xkb_state_events(
            xkb_before, xkb_after, key_event ? detail : 0, type,
            0, 0, events)) {
        return EventDelivery::queue_full;
    }
    const std::size_t xi2_start = events.size();
    if (!append_xi2_input_events(
            type, event.detail, detail, source, event_x, event_y,
            state_before, xkb_before, 0, grab, events)) {
        return EventDelivery::queue_full;
    }
    const bool xi2_delivered = events.size() != xi2_start;
    if (!queue_events_atomically(events))
        return EventDelivery::queue_full;
    const EventDelivery delivered =
        crossing == EventDelivery::delivered ||
        grab_crossing == EventDelivery::delivered ||
        grab_focus == EventDelivery::delivered ||
        routed == EventDelivery::delivered || xi2_delivered
        ? EventDelivery::delivered
        : EventDelivery::no_recipient;

    if (key_event) {
        input_.pressed_keys = prospective_keys;
        input_.xkb = prospective_xkb;
    }
    else if (button_event) {
        input_.pressed_buttons = prospective_buttons;
    }
    else if (motion_event) {
        input_.pointer_x = event_x;
        input_.pointer_y = event_y;
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
            detail, clock_.now() + std::chrono::milliseconds{
                input_.xkb.controls.repeat_delay}};
    }
    else if (type == 3 && key_repeat_ && key_repeat_->key == detail) {
        key_repeat_.reset();
    }
    displayed_cursor_ = prospective_cursor;
    return cursor == EventDelivery::delivered
        ? EventDelivery::delivered
        : delivered;
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
    xi2_selections_.erase(
        std::remove_if(
            xi2_selections_.begin(), xi2_selections_.end(),
            [id](const Xi2EventSelection &selection) {
                return selection.window == id;
            }),
        xi2_selections_.end());
    xfixes_selection_inputs_.erase(
        std::remove_if(
            xfixes_selection_inputs_.begin(), xfixes_selection_inputs_.end(),
            [id](const XFixesSelectionSubscription &entry) {
                return entry.window == id;
            }),
        xfixes_selection_inputs_.end());
    xfixes_cursor_inputs_.erase(
        std::remove_if(
            xfixes_cursor_inputs_.begin(), xfixes_cursor_inputs_.end(),
            [id](const XFixesCursorSubscription &entry) {
                return entry.window == id;
            }),
        xfixes_cursor_inputs_.end());
    randr_.subscriptions.erase(
        std::remove_if(
            randr_.subscriptions.begin(), randr_.subscriptions.end(),
            [id](const RandrSubscription &entry) {
                return entry.window == id;
            }),
        randr_.subscriptions.end());
    for (auto subscription = present_subscriptions_.begin();
         subscription != present_subscriptions_.end();) {
        if (subscription->window != id) {
            ++subscription;
            continue;
        }
        static_cast<void>(resources_.erase(subscription->id));
        subscription = present_subscriptions_.erase(subscription);
    }
    for (auto operation = present_operations_.begin();
         operation != present_operations_.end();) {
        if (operation->window == id) {
            if (operation->idle_fence != 0)
                static_cast<void>(trigger_sync_fence(
                    operation->idle_fence));
            operation = present_operations_.erase(operation);
            continue;
        }
        operation->notifies.erase(
            std::remove_if(
                operation->notifies.begin(), operation->notifies.end(),
                [id](const PresentNotify &notify) {
                    return notify.window == id;
                }),
            operation->notifies.end());
        ++operation;
    }
    composite_redirects_.erase(
        std::remove_if(
            composite_redirects_.begin(), composite_redirects_.end(),
            [id](const CompositeRedirect &entry) {
                return entry.window == id;
            }),
        composite_redirects_.end());
    for (auto save_set = save_sets_.begin(); save_set != save_sets_.end();) {
        auto &entries = save_set->second;
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                           [id](const SaveSetEntry &entry) {
                               return entry.window == id;
                           }),
            entries.end());
        if (entries.empty())
            save_set = save_sets_.erase(save_set);
        else
            ++save_set;
    }
    for (const auto &property : found->second.properties)
        property_bytes_ -= property.second.data.size();
    for (auto damage = damages_.begin(); damage != damages_.end();) {
        if (damage->second.drawable != id) {
            ++damage;
            continue;
        }
        static_cast<void>(resources_.erase(damage->first));
        damage = damages_.erase(damage);
    }
    for (auto picture = render_pictures_.begin();
         picture != render_pictures_.end();) {
        const auto *drawable = std::get_if<RenderDrawableSource>(
            &picture->second->source);
        if (drawable != nullptr && !drawable->pixmap &&
            drawable->drawable == id) {
            static_cast<void>(resources_.erase(picture->first));
            picture = render_pictures_.erase(picture);
        }
        else {
            ++picture;
        }
    }
    if (auto *parent = window(parent_id)) {
        parent->children.erase(
            std::remove(parent->children.begin(), parent->children.end(), id),
            parent->children.end());
    }
    windows_.erase(found);
    static_cast<void>(resources_.erase(id));
    invalidate_scene();
    static_cast<void>(cursor_maybe_changed());
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
    const auto prospective_cursor = current_cursor_for(
        new_pointer_window,
        pointer_grab_lost || !input_.pointer_grab
            ? nullptr
            : &*input_.pointer_grab);
    const EventDelivery cursor =
        pointer_ungrab == EventDelivery::queue_full ||
            keyboard_ungrab == EventDelivery::queue_full ||
            focus == EventDelivery::queue_full ||
            unmap_crossing == EventDelivery::queue_full ||
            map_crossing == EventDelivery::queue_full
        ? EventDelivery::queue_full
        : append_cursor_change(prospective_cursor, events);

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
        cursor == EventDelivery::queue_full ||
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
    displayed_cursor_ = prospective_cursor;
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
            std::vector<PlannedEvent> events;
            try {
                for (const auto &subscription : xfixes_selection_inputs_) {
                    if (subscription.selection != selection.first ||
                        (subscription.event_mask & 2U) == 0) {
                        continue;
                    }
                    events.emplace_back(
                        subscription.client,
                        XFixesSelectionNotifyEvent{
                            1, subscription.window, 0, selection.first,
                            current_time_, selection.second.changed_at});
                }
                static_cast<void>(queue_events_atomically(events));
            }
            catch (const std::bad_alloc &) {
            }
            selection.second.window = 0;
            selection.second.client = 0;
            selection.second.changed_at = current_time_;
        }
    }
}

void
ServerState::apply_save_set(std::uint32_t owner)
{
    const auto found = save_sets_.find(owner);
    if (found == save_sets_.end())
        return;
    std::vector<SaveSetEntry> entries = std::move(found->second);
    save_sets_.erase(found);
    const auto set_mapped_without_failure = [this](WindowRecord &candidate,
                                                   bool mapped) {
        if (set_window_mapped(candidate, mapped) !=
            EventDelivery::queue_full) {
            return;
        }
        candidate.mapped = mapped;
        if (!mapped) {
            if (input_.focus.kind == FocusKind::window &&
                map_state(input_.focus.window) != 2) {
                input_.focus = reverted_focus_state();
            }
            if (input_.pointer_grab &&
                (map_state(input_.pointer_grab->window) != 2 ||
                 (input_.pointer_grab->confine_to != 0 &&
                  map_state(input_.pointer_grab->confine_to) != 2))) {
                input_.pointer_grab.reset();
            }
            if (input_.keyboard_grab &&
                map_state(input_.keyboard_grab->window) != 2) {
                input_.keyboard_grab.reset();
            }
        }
        invalidate_scene();
        static_cast<void>(cursor_maybe_changed());
    };
    const auto reparent_without_failure =
        [this](WindowRecord &candidate, std::uint32_t parent_id,
               std::int16_t x, std::int16_t y) {
            if (reparent_window(candidate.id, parent_id, x, y) !=
                ReparentUpdate::queue_full) {
                return;
            }
            auto *old_parent = window(candidate.parent);
            auto *new_parent = window(parent_id);
            if (old_parent == nullptr || new_parent == nullptr)
                return;
            try {
                new_parent->children.reserve(new_parent->children.size() + 1);
            }
            catch (const std::bad_alloc &) {
                return;
            }
            old_parent->children.erase(
                std::remove(old_parent->children.begin(),
                            old_parent->children.end(), candidate.id),
                old_parent->children.end());
            new_parent->children.push_back(candidate.id);
            candidate.parent = parent_id;
            candidate.x = x;
            candidate.y = y;
            invalidate_scene();
            static_cast<void>(cursor_maybe_changed());
        };
    for (const auto &entry : entries) {
        auto *candidate = window(entry.window);
        if (candidate == nullptr || candidate->owner == owner)
            continue;
        std::uint32_t parent_id = entry.to_root
            ? root_window_id
            : candidate->parent;
        while (parent_id != 0 && parent_id != root_window_id) {
            const auto *parent = window(parent_id);
            if (parent == nullptr || parent->owner != owner)
                break;
            parent_id = parent->parent;
        }
        if (parent_id == 0 || window(parent_id) == nullptr)
            parent_id = root_window_id;
        if (candidate->parent != parent_id) {
            const auto absolute = absolute_position(candidate->id);
            const auto parent_absolute = absolute_position(parent_id);
            const auto x = wire_coordinate(
                absolute.first - parent_absolute.first);
            const auto y = wire_coordinate(
                absolute.second - parent_absolute.second);
            if (!entry.map)
                set_mapped_without_failure(*candidate, false);
            reparent_without_failure(*candidate, parent_id, x, y);
            candidate = window(entry.window);
            if (candidate == nullptr)
                continue;
        }
        if (entry.map)
            set_mapped_without_failure(*candidate, true);
    }
}

void
ServerState::constrain_pointer_by_barriers(
    std::int32_t old_x, std::int32_t old_y,
    std::int32_t &new_x, std::int32_t &new_y) const noexcept
{
    for (const auto &entry : xfixes_barriers_) {
        const auto &barrier = entry.second;
        if (!barrier.devices.empty() &&
            std::find(barrier.devices.begin(), barrier.devices.end(), 2) ==
                barrier.devices.end()) {
            continue;
        }
        if (barrier.x1 == barrier.x2) {
            const std::int32_t coordinate = barrier.x1;
            const std::int32_t low = std::min<std::int32_t>(
                barrier.y1, barrier.y2);
            const std::int32_t high = std::max<std::int32_t>(
                barrier.y1, barrier.y2);
            const bool meets_span =
                std::max(std::min(old_y, new_y), low) <=
                std::min(std::max(old_y, new_y), high);
            if (!meets_span)
                continue;
            if (old_x < coordinate && new_x >= coordinate &&
                (barrier.directions & 1U) == 0) {
                new_x = coordinate - 1;
            }
            else if (old_x >= coordinate && new_x < coordinate &&
                     (barrier.directions & 4U) == 0) {
                new_x = coordinate;
            }
        }
        else if (barrier.y1 == barrier.y2) {
            const std::int32_t coordinate = barrier.y1;
            const std::int32_t low = std::min<std::int32_t>(
                barrier.x1, barrier.x2);
            const std::int32_t high = std::max<std::int32_t>(
                barrier.x1, barrier.x2);
            const bool meets_span =
                std::max(std::min(old_x, new_x), low) <=
                std::min(std::max(old_x, new_x), high);
            if (!meets_span)
                continue;
            if (old_y < coordinate && new_y >= coordinate &&
                (barrier.directions & 2U) == 0) {
                new_y = coordinate - 1;
            }
            else if (old_y >= coordinate && new_y < coordinate &&
                     (barrier.directions & 8U) == 0) {
                new_y = coordinate;
            }
        }
    }
}

void
ServerState::disconnect_client(std::uint32_t owner)
{
    apply_save_set(owner);
    for (auto &selection : selections_) {
        if (selection.second.client != owner)
            continue;
        std::vector<PlannedEvent> events;
        try {
            for (const auto &subscription : xfixes_selection_inputs_) {
                if (subscription.client == owner ||
                    subscription.selection != selection.first ||
                    (subscription.event_mask & 4U) == 0) {
                    continue;
                }
                events.emplace_back(
                    subscription.client,
                    XFixesSelectionNotifyEvent{
                        2, subscription.window, 0, selection.first,
                        current_time_, selection.second.changed_at});
            }
            static_cast<void>(queue_events_atomically(events));
        }
        catch (const std::bad_alloc &) {
        }
    }
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
    xfixes_selection_inputs_.erase(
        std::remove_if(
            xfixes_selection_inputs_.begin(), xfixes_selection_inputs_.end(),
            [owner](const XFixesSelectionSubscription &entry) {
                return entry.client == owner;
            }),
        xfixes_selection_inputs_.end());
    xfixes_cursor_inputs_.erase(
        std::remove_if(
            xfixes_cursor_inputs_.begin(), xfixes_cursor_inputs_.end(),
            [owner](const XFixesCursorSubscription &entry) {
                return entry.client == owner;
            }),
        xfixes_cursor_inputs_.end());
    randr_.subscriptions.erase(
        std::remove_if(
            randr_.subscriptions.begin(), randr_.subscriptions.end(),
            [owner](const RandrSubscription &entry) {
                return entry.client == owner;
            }),
        randr_.subscriptions.end());
    for (auto subscription = present_subscriptions_.begin();
         subscription != present_subscriptions_.end();) {
        if (subscription->owner != owner) {
            ++subscription;
            continue;
        }
        static_cast<void>(resources_.erase(subscription->id));
        subscription = present_subscriptions_.erase(subscription);
    }
    present_operations_.erase(
        std::remove_if(
            present_operations_.begin(), present_operations_.end(),
            [owner](const PresentOperation &operation) {
                return operation.owner == owner;
            }),
        present_operations_.end());
    xkb_selections_.erase(
        std::remove_if(
            xkb_selections_.begin(), xkb_selections_.end(),
            [owner](const XkbEventSelection &selection) {
                return selection.owner == owner;
            }),
        xkb_selections_.end());
    xkb_client_flags_.erase(owner);
    xi2_selections_.erase(
        std::remove_if(
            xi2_selections_.begin(), xi2_selections_.end(),
            [owner](const Xi2EventSelection &selection) {
                return selection.owner == owner;
            }),
        xi2_selections_.end());
    cursor_hide_counts_.erase(owner);
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
    const auto pictures =
        resources_.owned_by(owner, ResourceKind::render_picture);
    for (const auto id : pictures)
        static_cast<void>(erase_render_picture(id));
    const auto glyph_sets =
        resources_.owned_by(owner, ResourceKind::render_glyph_set);
    for (const auto id : glyph_sets)
        static_cast<void>(erase_render_glyph_set(id));
    const auto cursors = resources_.owned_by(owner, ResourceKind::cursor);
    for (const auto id : cursors)
        static_cast<void>(erase_cursor(id));
    const auto damages = resources_.owned_by(owner, ResourceKind::damage);
    for (const auto id : damages)
        static_cast<void>(erase_damage(id));
    while (true) {
        const auto redirect = std::find_if(
            composite_redirects_.begin(), composite_redirects_.end(),
            [owner](const CompositeRedirect &entry) {
                return entry.owner == owner;
            });
        if (redirect == composite_redirects_.end())
            break;
        const CompositeRedirect removing = *redirect;
        const auto removed = unredirect_window(
            owner, removing.window, removing.subwindows,
            removing.update);
        if (removed != CompositeUpdate::updated) {
            const auto stale = std::find_if(
                composite_redirects_.begin(), composite_redirects_.end(),
                [&removing](const CompositeRedirect &entry) {
                    return entry.owner == removing.owner &&
                        entry.window == removing.window &&
                        entry.update == removing.update &&
                        entry.subwindows == removing.subwindows;
                });
            if (stale != composite_redirects_.end())
                composite_redirects_.erase(stale);
            invalidate_scene();
        }
    }
    const auto regions =
        resources_.owned_by(owner, ResourceKind::xfixes_region);
    for (const auto id : regions)
        static_cast<void>(erase_xfixes_region(id));
    const auto barriers =
        resources_.owned_by(owner, ResourceKind::xfixes_barrier);
    for (const auto id : barriers)
        static_cast<void>(erase_xfixes_barrier(id, owner));
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
