#include "viewer_transport.hpp"

#include <xcb/xcb.h>
#include <xcb/xtest.h>
#if XMIN_VIEWER_HAVE_DAMAGE
#include <xcb/damage.h>
#endif
#if XMIN_VIEWER_HAVE_SHM
#include <xcb/shm.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xmin::viewer {
namespace {

class ScopedAuthority {
public:
    explicit ScopedAuthority(const std::string &authority)
    {
        const char *current = std::getenv("XAUTHORITY");
        if (current != nullptr) {
            previous_ = current;
            had_previous_ = true;
        }
        if (!authority.empty())
            changed_ = ::setenv("XAUTHORITY", authority.c_str(), 1) == 0;
    }

    ~ScopedAuthority()
    {
        if (!changed_)
            return;
        if (had_previous_)
            static_cast<void>(::setenv("XAUTHORITY", previous_.c_str(), 1));
        else
            static_cast<void>(::unsetenv("XAUTHORITY"));
    }

private:
    std::string previous_;
    bool had_previous_ = false;
    bool changed_ = false;
};

class XcbTransport final : public GuestTransport {
    struct FrameArea {
        std::uint16_t left = 0;
        std::uint16_t top = 0;
        std::uint16_t right = 0;
        std::uint16_t bottom = 0;
    };

public:
    XcbTransport(const std::string &display, const std::string &authority,
                 bool allow_shared_memory)
    {
        int screen_index = 0;
        {
            const ScopedAuthority selected(authority);
            connection_ = xcb_connect(display.c_str(), &screen_index);
        }
        if (connection_ == nullptr || xcb_connection_has_error(connection_) != 0) {
            error_ = "cannot connect to Xmin display " + display;
            return;
        }
        auto screen = xcb_setup_roots_iterator(xcb_get_setup(connection_));
        while (screen.rem != 0 && screen_index > 0) {
            xcb_screen_next(&screen);
            --screen_index;
        }
        if (screen.rem == 0 || screen.data == nullptr) {
            error_ = "Xmin display does not contain the selected screen";
            return;
        }
        root_ = screen.data->root;
        width_ = screen.data->width_in_pixels;
        height_ = screen.data->height_in_pixels;
        depth_ = screen.data->root_depth;
        if ((depth_ != 24 && depth_ != 32) || width_ == 0 || height_ == 0) {
            error_ = "viewer requires a non-empty 24- or 32-bit Xmin screen";
            return;
        }
        const std::uint64_t bytes =
            static_cast<std::uint64_t>(width_) * height_ * 4U;
        if (bytes > std::numeric_limits<std::uint32_t>::max()) {
            error_ = "Xmin screen is too large for the viewer transport";
            return;
        }
        frame_size_ = static_cast<std::size_t>(bytes);
        mark_full_frame();
#if XMIN_VIEWER_HAVE_SHM
        if (allow_shared_memory)
            static_cast<void>(initialize_shared_frame());
#else
        static_cast<void>(allow_shared_memory);
#endif
        initialize_frame_notifications();
        valid_ = true;
    }

    ~XcbTransport() override
    {
#if XMIN_VIEWER_HAVE_SHM
        release_shared_frame();
#endif
        if (connection_ != nullptr)
            xcb_disconnect(connection_);
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::uint16_t width() const noexcept override
    {
        return width_;
    }
    [[nodiscard]] std::uint16_t height() const noexcept override
    {
        return height_;
    }
    [[nodiscard]] bool using_shared_memory() const noexcept override
    {
#if XMIN_VIEWER_HAVE_SHM
        return shared_pixels_ != nullptr;
#else
        return false;
#endif
    }
    [[nodiscard]] const std::string &error() const noexcept override
    {
        return error_;
    }

    bool frame_pending() override
    {
#if XMIN_VIEWER_HAVE_DAMAGE
        if (damage_first_event_ != 0) {
            while (xcb_generic_event_t *event =
                       xcb_poll_for_event(connection_)) {
                const std::uint8_t type =
                    static_cast<std::uint8_t>(event->response_type & 0x7fU);
                if (type == damage_first_event_ + XCB_DAMAGE_NOTIFY) {
                    const auto *notify =
                        reinterpret_cast<xcb_damage_notify_event_t *>(event);
                    xcb_damage_subtract(connection_, notify->damage,
                                        XCB_NONE, XCB_NONE);
                    mark_frame(
                        static_cast<std::int32_t>(notify->geometry.x) +
                            notify->area.x,
                        static_cast<std::int32_t>(notify->geometry.y) +
                            notify->area.y,
                        notify->area.width, notify->area.height);
                }
                else if (type == XCB_CREATE_NOTIFY) {
                    const auto *created =
                        reinterpret_cast<xcb_create_notify_event_t *>(event);
                    watch_window_tree(created->window);
                    mark_full_frame();
                }
                else if (type == XCB_DESTROY_NOTIFY) {
                    const auto *destroyed =
                        reinterpret_cast<xcb_destroy_notify_event_t *>(event);
                    watched_windows_.erase(destroyed->window);
                    damages_.erase(destroyed->window);
                    mark_full_frame();
                }
                else if (type == XCB_MAP_NOTIFY) {
                    const auto *mapped =
                        reinterpret_cast<xcb_map_notify_event_t *>(event);
                    // CreateNotify is the normal discovery path, but also
                    // attach here so a late viewer or an imperfect server
                    // cannot leave a newly mapped window permanently
                    // outside DAMAGE tracking.
                    watch_window_tree(mapped->window);
                    mark_full_frame();
                }
                else if (type == XCB_REPARENT_NOTIFY) {
                    const auto *reparented =
                        reinterpret_cast<xcb_reparent_notify_event_t *>(event);
                    watch_window_tree(reparented->window);
                    mark_full_frame();
                }
                else if (type == XCB_UNMAP_NOTIFY ||
                         type == XCB_CONFIGURE_NOTIFY ||
                         type == XCB_GRAVITY_NOTIFY ||
                         type == XCB_CIRCULATE_NOTIFY) {
                    mark_full_frame();
                }
                std::free(event);
            }
            if (!pending_frames_.empty())
                static_cast<void>(xcb_flush(connection_));
            return !pending_frames_.empty();
        }
#endif
        mark_full_frame();
        return true;
    }

    bool capture(FrameView &frame) override
    {
        if (pending_frames_.empty())
            return false;
        const FrameArea area = pending_frames_.front();
        const std::uint16_t frame_width = static_cast<std::uint16_t>(
            area.right - area.left);
        const std::uint16_t frame_height = static_cast<std::uint16_t>(
            area.bottom - area.top);
        const std::size_t capture_size =
            static_cast<std::size_t>(frame_width) * frame_height * 4U;
#if XMIN_VIEWER_HAVE_SHM
        if (shared_pixels_ != nullptr) {
            xcb_generic_error_t *protocol_error = nullptr;
            auto *reply = xcb_shm_get_image_reply(
                connection_,
                xcb_shm_get_image(connection_, root_, area.left, area.top,
                                  frame_width, frame_height,
                                  0xffffffffU, XCB_IMAGE_FORMAT_Z_PIXMAP,
                                  segment_, 0),
                &protocol_error);
            const bool success = reply != nullptr && protocol_error == nullptr &&
                reply->size >= capture_size;
            std::free(protocol_error);
            std::free(reply);
            if (success) {
                pending_frames_.erase(pending_frames_.begin());
                frame = {shared_pixels_, area.left, area.top,
                         frame_width, frame_height,
                         static_cast<std::size_t>(frame_width) * 4U,
                         !pending_frames_.empty()};
                return true;
            }
            release_shared_frame();
        }
#endif
        try {
            copied_pixels_.resize(capture_size);
        }
        catch (const std::bad_alloc &) {
            error_ = "cannot allocate the viewer frame buffer";
            return false;
        }

        // Keep fallback replies below Xmin's bounded per-connection output
        // buffer.  This path is also the portable transport for platforms
        // where an fd-backed MIT-SHM mapping is unavailable.
        constexpr std::size_t maximum_tile_bytes = 512U * 1024U;
        const std::size_t stride =
            static_cast<std::size_t>(frame_width) * 4U;
        const std::size_t rows_per_tile =
            std::max<std::size_t>(1, maximum_tile_bytes / stride);
        for (std::uint32_t y = 0; y < frame_height;) {
            const std::uint16_t rows = static_cast<std::uint16_t>(
                std::min<std::size_t>(rows_per_tile, frame_height - y));
            xcb_generic_error_t *protocol_error = nullptr;
            const auto cookie = xcb_get_image(
                connection_, XCB_IMAGE_FORMAT_Z_PIXMAP, root_,
                static_cast<std::int16_t>(area.left),
                static_cast<std::int16_t>(area.top + y), frame_width, rows,
                0xffffffffU);
            auto *reply = xcb_get_image_reply(
                connection_, cookie, &protocol_error);
            const std::size_t expected = stride * rows;
            if (reply == nullptr || protocol_error != nullptr ||
                xcb_get_image_data_length(reply) <
                    static_cast<int>(expected)) {
                error_ = "cannot capture the Xmin root image (tile " +
                    std::to_string(area.left) + "," +
                    std::to_string(area.top + y) + " " +
                    std::to_string(frame_width) + "x" +
                    std::to_string(rows) + ", reply " +
                    (reply != nullptr ? "present" : "missing") +
                    ", X11 error " +
                    std::to_string(protocol_error != nullptr
                                       ? protocol_error->error_code
                                       : 0) +
                    ", connection error " +
                    std::to_string(xcb_connection_has_error(connection_)) +
                    ", request sequence " +
                    std::to_string(cookie.sequence) +
                    ", bytes " +
                    std::to_string(reply != nullptr
                                       ? xcb_get_image_data_length(reply)
                                       : 0) + "/" +
                    std::to_string(expected) + ")";
                std::free(protocol_error);
                std::free(reply);
                return false;
            }
            std::memcpy(copied_pixels_.data() + stride * y,
                        xcb_get_image_data(reply), expected);
            std::free(protocol_error);
            std::free(reply);
            y += rows;
        }
        pending_frames_.erase(pending_frames_.begin());
        frame = {copied_pixels_.data(), area.left, area.top,
                 frame_width, frame_height, stride,
                 !pending_frames_.empty()};
        return true;
    }

    void key(std::uint8_t keycode, bool pressed) override
    {
        fake_input(pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode, 0, 0);
    }

    void button(std::uint8_t button, bool pressed) override
    {
        fake_input(pressed ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE,
                   button, 0, 0);
    }

    void pointer(std::int16_t x, std::int16_t y) override
    {
        fake_input(XCB_MOTION_NOTIFY, 0, x, y);
    }

    void flush() override
    {
        if (connection_ != nullptr)
            static_cast<void>(xcb_flush(connection_));
    }

private:
    void initialize_frame_notifications()
    {
#if XMIN_VIEWER_HAVE_DAMAGE
        const auto *extension =
            xcb_get_extension_data(connection_, &xcb_damage_id);
        if (extension == nullptr || extension->present == 0)
            return;
        xcb_generic_error_t *protocol_error = nullptr;
        auto *version = xcb_damage_query_version_reply(
            connection_, xcb_damage_query_version(connection_, 1, 1),
            &protocol_error);
        const bool supported = version != nullptr && protocol_error == nullptr;
        std::free(protocol_error);
        std::free(version);
        if (!supported)
            return;
        damage_first_event_ = extension->first_event;
        watch_window_tree(root_);
        static_cast<void>(xcb_flush(connection_));
#endif
    }

#if XMIN_VIEWER_HAVE_DAMAGE
    bool watch_window(xcb_window_t window)
    {
        xcb_generic_error_t *protocol_error = nullptr;
        auto *attributes = xcb_get_window_attributes_reply(
            connection_, xcb_get_window_attributes(connection_, window),
            &protocol_error);
        if (attributes == nullptr || protocol_error != nullptr) {
            std::free(protocol_error);
            std::free(attributes);
            return false;
        }

        const std::uint32_t event_mask =
            XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
        protocol_error = xcb_request_check(
            connection_, xcb_change_window_attributes_checked(
                connection_, window, XCB_CW_EVENT_MASK, &event_mask));
        if (protocol_error != nullptr) {
            std::free(protocol_error);
            std::free(attributes);
            return false;
        }

        xcb_damage_damage_t damage = XCB_NONE;
        if (attributes->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT) {
            damage = xcb_generate_id(connection_);
            protocol_error = xcb_request_check(
                connection_, xcb_damage_create_checked(
                    connection_, damage, window,
                    XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES));
            if (protocol_error != nullptr) {
                std::free(protocol_error);
                std::free(attributes);
                return false;
            }
        }
        std::free(attributes);

        try {
            if (damage != XCB_NONE)
                damages_.emplace(window, damage);
            watched_windows_.insert(window);
        }
        catch (const std::bad_alloc &) {
            if (damage != XCB_NONE) {
                damages_.erase(window);
                xcb_damage_destroy(connection_, damage);
            }
            watched_windows_.erase(window);
            return false;
        }
        return true;
    }

    void watch_window_tree(xcb_window_t first)
    {
        std::vector<xcb_window_t> pending{first};
        while (!pending.empty()) {
            const xcb_window_t window = pending.back();
            pending.pop_back();
            if (watched_windows_.count(window) == 0)
                static_cast<void>(watch_window(window));

            auto *tree = xcb_query_tree_reply(
                connection_, xcb_query_tree(connection_, window), nullptr);
            if (tree == nullptr)
                continue;
            const auto *children = xcb_query_tree_children(tree);
            const int child_count = xcb_query_tree_children_length(tree);
            if (child_count > 0)
                pending.insert(pending.end(), children,
                               children + child_count);
            std::free(tree);
        }
    }
#endif

    void mark_full_frame() noexcept
    {
        pending_frames_.clear();
        if (width_ != 0 && height_ != 0)
            pending_frames_.push_back({0, 0, width_, height_});
    }

    void mark_frame(std::int32_t x, std::int32_t y,
                    std::uint32_t width, std::uint32_t height) noexcept
    {
        const std::int64_t left = std::max<std::int64_t>(0, x);
        const std::int64_t top = std::max<std::int64_t>(0, y);
        const std::int64_t right = std::min<std::int64_t>(
            width_, static_cast<std::int64_t>(x) + width);
        const std::int64_t bottom = std::min<std::int64_t>(
            height_, static_cast<std::int64_t>(y) + height);
        if (left >= right || top >= bottom)
            return;
        if (pending_frames_.size() == 1 &&
            pending_frames_.front().left == 0 &&
            pending_frames_.front().top == 0 &&
            pending_frames_.front().right == width_ &&
            pending_frames_.front().bottom == height_) {
            return;
        }
        FrameArea added{
            static_cast<std::uint16_t>(left),
            static_cast<std::uint16_t>(top),
            static_cast<std::uint16_t>(right),
            static_cast<std::uint16_t>(bottom)};
        for (std::size_t index = 0; index < pending_frames_.size();) {
            const auto &existing = pending_frames_[index];
            if (added.right < existing.left ||
                existing.right < added.left ||
                added.bottom < existing.top ||
                existing.bottom < added.top) {
                ++index;
                continue;
            }
            added.left = std::min(added.left, existing.left);
            added.top = std::min(added.top, existing.top);
            added.right = std::max(added.right, existing.right);
            added.bottom = std::max(added.bottom, existing.bottom);
            pending_frames_.erase(pending_frames_.begin() +
                                  static_cast<std::ptrdiff_t>(index));
        }
        pending_frames_.push_back(added);
        constexpr std::size_t maximum_dirty_rectangles = 8;
        if (pending_frames_.size() > maximum_dirty_rectangles)
            mark_full_frame();
    }

    void fake_input(std::uint8_t type, std::uint8_t detail,
                    std::int16_t x, std::int16_t y)
    {
        if (!valid_)
            return;
        xcb_test_fake_input(connection_, type, detail, XCB_CURRENT_TIME,
                            root_, x, y, 0);
    }

#if XMIN_VIEWER_HAVE_SHM
    bool initialize_shared_frame()
    {
        const auto *extension = xcb_get_extension_data(connection_, &xcb_shm_id);
        if (extension == nullptr || extension->present == 0)
            return false;
        auto *version = xcb_shm_query_version_reply(
            connection_, xcb_shm_query_version(connection_), nullptr);
        const bool create_segment_supported = version != nullptr &&
            version->major_version == 1 && version->minor_version >= 2;
        std::free(version);
        if (!create_segment_supported)
            return false;

        segment_ = xcb_generate_id(connection_);
        xcb_generic_error_t *protocol_error = nullptr;
        auto *reply = xcb_shm_create_segment_reply(
            connection_, xcb_shm_create_segment(
                connection_, segment_, static_cast<std::uint32_t>(frame_size_),
                0),
            &protocol_error);
        if (reply == nullptr || protocol_error != nullptr || reply->nfd != 1) {
            std::free(protocol_error);
            std::free(reply);
            segment_ = 0;
            return false;
        }
        int *descriptors = xcb_shm_create_segment_reply_fds(connection_, reply);
        const int descriptor = descriptors[0];
        void *mapping = ::mmap(nullptr, frame_size_, PROT_READ | PROT_WRITE,
                               MAP_SHARED, descriptor, 0);
        static_cast<void>(::close(descriptor));
        std::free(protocol_error);
        std::free(reply);
        if (mapping == MAP_FAILED) {
            xcb_shm_detach(connection_, segment_);
            segment_ = 0;
            return false;
        }
        shared_pixels_ = static_cast<std::uint8_t *>(mapping);
        return true;
    }

    void release_shared_frame()
    {
        if (segment_ != 0 && connection_ != nullptr)
            xcb_shm_detach(connection_, segment_);
        if (shared_pixels_ != nullptr)
            static_cast<void>(::munmap(shared_pixels_, frame_size_));
        segment_ = 0;
        shared_pixels_ = nullptr;
    }
#endif

    xcb_connection_t *connection_ = nullptr;
    xcb_window_t root_ = XCB_NONE;
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    std::uint8_t depth_ = 0;
    std::size_t frame_size_ = 0;
    bool valid_ = false;
    std::string error_;
    std::vector<std::uint8_t> copied_pixels_;
#if XMIN_VIEWER_HAVE_DAMAGE
    std::uint8_t damage_first_event_ = 0;
    std::unordered_set<xcb_window_t> watched_windows_;
    std::unordered_map<xcb_window_t, xcb_damage_damage_t> damages_;
#endif
    std::vector<FrameArea> pending_frames_;
#if XMIN_VIEWER_HAVE_SHM
    xcb_shm_seg_t segment_ = 0;
    std::uint8_t *shared_pixels_ = nullptr;
#endif
};

} // namespace

std::unique_ptr<GuestTransport>
create_guest_transport(const std::string &display,
                       const std::string &authority,
                       bool allow_shared_memory, std::string &error)
{
    auto transport = std::make_unique<XcbTransport>(
        display, authority, allow_shared_memory);
    if (!transport->valid()) {
        error = transport->error();
        return {};
    }
    return transport;
}

} // namespace xmin::viewer
