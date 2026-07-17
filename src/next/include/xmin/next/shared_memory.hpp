#ifndef XMIN_NEXT_SHARED_MEMORY_HPP
#define XMIN_NEXT_SHARED_MEMORY_HPP

#include "xmin/next/result.hpp"
#include "xmin/next/unique_fd.hpp"

#include <cstddef>

namespace xmin::next {

class SharedMemory {
public:
    SharedMemory() noexcept = default;
    ~SharedMemory();

    SharedMemory(const SharedMemory &) = delete;
    SharedMemory &operator=(const SharedMemory &) = delete;
    SharedMemory(SharedMemory &&other) noexcept;
    SharedMemory &operator=(SharedMemory &&other) noexcept;

    [[nodiscard]] static Result<SharedMemory>
    attach_sysv(int id, bool read_only);
    [[nodiscard]] static Result<SharedMemory>
    attach_fd(UniqueFd fd, bool read_only);
    [[nodiscard]] static Result<SharedMemory>
    create(std::size_t size, bool read_only, UniqueFd &client_fd);

    [[nodiscard]] const std::byte *data() const noexcept
    {
        return static_cast<const std::byte *>(address_);
    }
    [[nodiscard]] std::byte *writable_data() noexcept
    {
        return read_only_ ? nullptr : static_cast<std::byte *>(address_);
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool read_only() const noexcept { return read_only_; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return address_ != nullptr;
    }

private:
    enum class Mapping {
        none,
        system_v,
        file,
    };

    SharedMemory(void *address, std::size_t size, bool read_only,
                 Mapping mapping, UniqueFd fd = {}) noexcept;
    void reset() noexcept;

    void *address_ = nullptr;
    std::size_t size_ = 0;
    bool read_only_ = false;
    Mapping mapping_ = Mapping::none;
    UniqueFd fd_;
};

} // namespace xmin::next

#endif
