#include "xmin/config.h"
#include "xmin/server/shared_memory.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#if XMIN_HAVE_MITSHM
#include <sys/ipc.h>
#include <sys/shm.h>
#endif
#include <unistd.h>
#include <utility>

namespace xmin::server {
namespace {

constexpr std::size_t maximum_mapping_size = 256U * 1024U * 1024U;

Result<SharedMemory>
mapping_failure(const char *operation)
{
    return Result<SharedMemory>::failure(
        ErrorCode::io, std::string(operation) + ": " + std::strerror(errno));
}

bool
valid_mapping_size(std::uintmax_t size) noexcept
{
    return size != 0 && size <= maximum_mapping_size &&
        size <= std::numeric_limits<std::size_t>::max();
}

void
set_close_on_exec(int fd) noexcept
{
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0)
        static_cast<void>(::fcntl(fd, F_SETFD, flags | FD_CLOEXEC));
}

} // namespace

SharedMemory::SharedMemory(void *address, std::size_t size, bool read_only,
                           Mapping mapping, UniqueFd fd) noexcept
    : address_(address), size_(size), read_only_(read_only), mapping_(mapping),
      fd_(std::move(fd))
{}

SharedMemory::~SharedMemory()
{
    reset();
}

SharedMemory::SharedMemory(SharedMemory &&other) noexcept
    : address_(std::exchange(other.address_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      read_only_(std::exchange(other.read_only_, false)),
      mapping_(std::exchange(other.mapping_, Mapping::none)),
      fd_(std::move(other.fd_))
{}

SharedMemory &
SharedMemory::operator=(SharedMemory &&other) noexcept
{
    if (this == &other)
        return *this;
    reset();
    address_ = std::exchange(other.address_, nullptr);
    size_ = std::exchange(other.size_, 0);
    read_only_ = std::exchange(other.read_only_, false);
    mapping_ = std::exchange(other.mapping_, Mapping::none);
    fd_ = std::move(other.fd_);
    return *this;
}

void
SharedMemory::reset() noexcept
{
    if (address_ != nullptr) {
        if (mapping_ == Mapping::system_v) {
#if XMIN_HAVE_MITSHM
            static_cast<void>(::shmdt(address_));
#endif
        }
        else if (mapping_ == Mapping::file) {
            static_cast<void>(::munmap(address_, size_));
        }
    }
    address_ = nullptr;
    size_ = 0;
    read_only_ = false;
    mapping_ = Mapping::none;
    fd_.reset();
}

Result<SharedMemory>
SharedMemory::attach_sysv(int id, bool read_only)
{
#if XMIN_HAVE_MITSHM
    struct shmid_ds information {};
    if (::shmctl(id, IPC_STAT, &information) != 0)
        return mapping_failure("shmctl");
    if (!valid_mapping_size(information.shm_segsz)) {
        return Result<SharedMemory>::failure(
            ErrorCode::invalid_argument,
            "shared-memory segment has an unsupported size");
    }
    void *address = ::shmat(id, nullptr, read_only ? SHM_RDONLY : 0);
    if (address == reinterpret_cast<void *>(-1))
        return mapping_failure("shmat");
    return Result<SharedMemory>::success(SharedMemory(
        address, static_cast<std::size_t>(information.shm_segsz), read_only,
        Mapping::system_v));
#else
    static_cast<void>(id);
    static_cast<void>(read_only);
    return Result<SharedMemory>::failure(
        ErrorCode::invalid_argument, "System V shared memory is unavailable");
#endif
}

Result<SharedMemory>
SharedMemory::attach_fd(UniqueFd fd, bool read_only)
{
    if (!fd) {
        return Result<SharedMemory>::failure(
            ErrorCode::invalid_argument, "missing shared-memory descriptor");
    }
    struct stat information {};
    if (::fstat(fd.get(), &information) != 0)
        return mapping_failure("fstat");
    if (information.st_size <= 0 ||
        !valid_mapping_size(static_cast<std::uintmax_t>(information.st_size))) {
        return Result<SharedMemory>::failure(
            ErrorCode::invalid_argument,
            "shared-memory descriptor has an unsupported size");
    }
    const int protection = PROT_READ | (read_only ? 0 : PROT_WRITE);
    void *address = ::mmap(nullptr, static_cast<std::size_t>(information.st_size),
                           protection, MAP_SHARED, fd.get(), 0);
    if (address == MAP_FAILED)
        return mapping_failure("mmap");
    return Result<SharedMemory>::success(SharedMemory(
        address, static_cast<std::size_t>(information.st_size), read_only,
        Mapping::file, std::move(fd)));
}

Result<SharedMemory>
SharedMemory::create(std::size_t size, bool read_only, UniqueFd &client_fd)
{
    if (!valid_mapping_size(size)) {
        return Result<SharedMemory>::failure(
            ErrorCode::invalid_argument,
            "requested shared-memory size is unsupported");
    }
    char path[] = "/tmp/xmin-shm-XXXXXX";
    UniqueFd server_fd(::mkstemp(path));
    if (!server_fd)
        return mapping_failure("mkstemp");
    static_cast<void>(::unlink(path));
    set_close_on_exec(server_fd.get());
    if (::ftruncate(server_fd.get(), static_cast<off_t>(size)) != 0)
        return mapping_failure("ftruncate");
    UniqueFd sent_fd(::dup(server_fd.get()));
    if (!sent_fd)
        return mapping_failure("dup");
    set_close_on_exec(sent_fd.get());
    auto mapped = attach_fd(std::move(server_fd), read_only);
    if (!mapped)
        return mapped;
    client_fd = std::move(sent_fd);
    return mapped;
}

} // namespace xmin::server
