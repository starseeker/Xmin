#ifndef XMIN_SERVER_DISPLAY_SOCKET_HPP
#define XMIN_SERVER_DISPLAY_SOCKET_HPP

#include "xmin/server/result.hpp"
#include "xmin/server/unique_fd.hpp"

#include <optional>
#include <string>
#include <sys/types.h>

namespace xmin::server {

class DisplaySocket {
public:
    static Result<DisplaySocket>
    open(std::optional<unsigned> requested_display,
         std::string runtime_root = "/tmp");

    DisplaySocket(DisplaySocket &&other) noexcept;
    DisplaySocket &operator=(DisplaySocket &&other) noexcept;
    ~DisplaySocket();

    DisplaySocket(const DisplaySocket &) = delete;
    DisplaySocket &operator=(const DisplaySocket &) = delete;

    [[nodiscard]] int fd() const noexcept { return listener_.get(); }
    [[nodiscard]] unsigned display() const noexcept { return display_; }
    [[nodiscard]] const std::string &socket_path() const noexcept
    {
        return socket_path_;
    }
    [[nodiscard]] const std::string &lock_path() const noexcept
    {
        return lock_path_;
    }

    Result<std::optional<UniqueFd>> accept_client();
    Result<void> notify_display(int descriptor) const;

private:
    DisplaySocket() = default;

    static Result<void> ensure_socket_directory(const std::string &root);
    Result<void> start(unsigned display, const std::string &root);
    Result<void> acquire_lock(const std::string &root);
    Result<void> bind_listener(const std::string &root);
    void cleanup() noexcept;

    UniqueFd listener_;
    UniqueFd lock_descriptor_;
    unsigned display_ = 0;
    std::string socket_path_;
    std::string lock_path_;
    dev_t socket_device_ = 0;
    ino_t socket_inode_ = 0;
    dev_t lock_device_ = 0;
    ino_t lock_inode_ = 0;
    bool owns_socket_ = false;
    bool owns_lock_ = false;
};

} // namespace xmin::server

#endif
