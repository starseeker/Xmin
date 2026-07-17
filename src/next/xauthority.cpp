#include "xmin/next/xauthority.hpp"

#include "xmin/config.h"
#include "xmin/next/unique_fd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#if XMIN_HAVE_GETRANDOM
#include <sys/random.h>
#endif

namespace xmin::next {
namespace {

constexpr std::uint16_t family_local = 256;
constexpr std::uint16_t family_wild = 65535;
constexpr std::size_t maximum_file_size = 1024U * 1024U;
constexpr std::size_t maximum_field_size = 65535U;
constexpr std::string_view cookie_protocol = "MIT-MAGIC-COOKIE-1";

Result<void>
write_all(int descriptor, const std::vector<std::uint8_t> &bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return Result<void>::failure(
            ErrorCode::io, std::string("write: ") + std::strerror(errno));
    }
    return Result<void>::success();
}

void
append_u16(std::vector<std::uint8_t> &bytes, std::size_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void
append_field(std::vector<std::uint8_t> &bytes, std::string_view field)
{
    append_u16(bytes, field.size());
    bytes.insert(bytes.end(), field.begin(), field.end());
}

void
append_field(
    std::vector<std::uint8_t> &bytes,
    const std::vector<std::uint8_t> &field)
{
    append_u16(bytes, field.size());
    bytes.insert(bytes.end(), field.begin(), field.end());
}

struct Record {
    std::uint16_t family = 0;
    std::string address;
    std::string number;
    std::string name;
    std::vector<std::uint8_t> data;
};

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t> &bytes) : bytes_(bytes) {}

    [[nodiscard]] bool empty() const noexcept { return offset_ == bytes_.size(); }

    std::optional<std::uint16_t> u16()
    {
        if (bytes_.size() - offset_ < 2)
            return std::nullopt;
        const auto value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes_[offset_]) << 8 |
            bytes_[offset_ + 1]);
        offset_ += 2;
        return value;
    }

    std::optional<std::vector<std::uint8_t>> field()
    {
        const auto size = u16();
        if (!size || *size > maximum_field_size ||
            bytes_.size() - offset_ < *size) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> value(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + *size));
        offset_ += *size;
        return value;
    }

private:
    const std::vector<std::uint8_t> &bytes_;
    std::size_t offset_ = 0;
};

std::string
as_string(const std::vector<std::uint8_t> &bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

Result<Record>
read_record(Reader &reader)
{
    const auto family = reader.u16();
    const auto address = reader.field();
    const auto number = reader.field();
    const auto name = reader.field();
    const auto data = reader.field();
    if (!family || !address || !number || !name || !data) {
        return Result<Record>::failure(
            ErrorCode::malformed, "truncated Xauthority record");
    }
    return Result<Record>::success(Record{
        *family,
        as_string(*address),
        as_string(*number),
        as_string(*name),
        *data,
    });
}

Result<std::vector<std::uint8_t>>
read_file(const std::string &path)
{
    UniqueFd descriptor(::open(path.c_str(), O_RDONLY));
    if (!descriptor) {
        return Result<std::vector<std::uint8_t>>::failure(
            ErrorCode::io,
            "open " + path + ": " + std::strerror(errno));
    }

    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0) {
        return Result<std::vector<std::uint8_t>>::failure(
            ErrorCode::io,
            "fstat " + path + ": " + std::strerror(errno));
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > maximum_file_size) {
        return Result<std::vector<std::uint8_t>>::failure(
            ErrorCode::invalid_argument,
            "Xauthority file must be a regular file no larger than 1 MiB");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(
            descriptor.get(), bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count == 0) {
            return Result<std::vector<std::uint8_t>>::failure(
                ErrorCode::unexpected_eof,
                "Xauthority file changed while it was being read");
        }
        return Result<std::vector<std::uint8_t>>::failure(
            ErrorCode::io,
            "read " + path + ": " + std::strerror(errno));
    }
    return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

std::string
local_hostname()
{
    std::array<char, 256> hostname{};
    if (::gethostname(hostname.data(), hostname.size()) != 0)
        return {};
    hostname.back() = '\0';
    return hostname.data();
}

int
match_score(const Record &record, std::string_view display,
            std::string_view hostname)
{
    if (record.name != cookie_protocol ||
        record.data.empty() || record.data.size() > 256) {
        return 0;
    }
    if (record.family == family_wild)
        return record.number.empty() || record.number == display ? 1 : 0;
    if (record.family != family_local || record.number != display)
        return 0;
    if (record.address.empty())
        return 2;
    if (!hostname.empty() && record.address == hostname)
        return 3;
    return 0;
}

} // namespace

Result<std::vector<std::uint8_t>>
load_xauthority_cookie(const std::string &path, unsigned display)
{
    auto bytes = read_file(path);
    if (!bytes) {
        return Result<std::vector<std::uint8_t>>::failure(
            bytes.error().code, bytes.error().message);
    }

    Reader reader(bytes.value());
    const std::string number = std::to_string(display);
    const std::string hostname = local_hostname();
    int best_score = 0;
    std::vector<std::uint8_t> best_cookie;
    while (!reader.empty()) {
        auto record = read_record(reader);
        if (!record) {
            return Result<std::vector<std::uint8_t>>::failure(
                record.error().code,
                path + ": " + record.error().message);
        }
        const int score = match_score(record.value(), number, hostname);
        if (score > best_score) {
            best_score = score;
            best_cookie = std::move(record.value().data);
        }
    }
    if (best_score == 0) {
        return Result<std::vector<std::uint8_t>>::failure(
            ErrorCode::invalid_argument,
            path + ": no matching MIT-MAGIC-COOKIE-1 record for display " +
                number);
    }
    return Result<std::vector<std::uint8_t>>::success(std::move(best_cookie));
}

Result<std::vector<std::uint8_t>>
secure_random_bytes(std::size_t size)
{
    if (size == 0 || size > maximum_field_size) {
        return Result<std::vector<std::uint8_t>>::failure(
            ErrorCode::invalid_argument, "invalid random byte count");
    }
    std::vector<std::uint8_t> bytes(size);
    std::size_t offset = 0;
#if XMIN_HAVE_GETRANDOM
    while (offset < bytes.size()) {
        const auto count = ::getrandom(
            bytes.data() + offset, bytes.size() - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return Result<std::vector<std::uint8_t>>::failure(
            ErrorCode::io,
            std::string("getrandom: ") + std::strerror(errno));
    }
#elif XMIN_HAVE_GETENTROPY
    while (offset < bytes.size()) {
        const std::size_t count =
            std::min<std::size_t>(256, bytes.size() - offset);
        if (::getentropy(bytes.data() + offset, count) != 0) {
            return Result<std::vector<std::uint8_t>>::failure(
                ErrorCode::io,
                std::string("getentropy: ") + std::strerror(errno));
        }
        offset += count;
    }
#elif XMIN_HAVE_ARC4RANDOM_BUF
    ::arc4random_buf(bytes.data(), bytes.size());
#else
#error "Xmin requires getrandom, getentropy, or arc4random_buf"
#endif
    return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

Result<void>
write_xauthority_cookie(
    const std::string &path, const std::vector<std::uint8_t> &cookie)
{
    if (path.empty() || cookie.empty() || cookie.size() > maximum_field_size) {
        return Result<void>::failure(
            ErrorCode::invalid_argument, "invalid Xauthority cookie");
    }
    std::vector<std::uint8_t> record;
    try {
        record.reserve(12 + cookie_protocol.size() + cookie.size());
        append_u16(record, family_wild);
        append_field(record, std::string_view{});
        append_field(record, std::string_view{});
        append_field(record, cookie_protocol);
        append_field(record, cookie);
    }
    catch (const std::bad_alloc &) {
        return Result<void>::failure(
            ErrorCode::io, "out of memory writing Xauthority");
    }

    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    UniqueFd descriptor(::open(path.c_str(), flags, 0600));
    if (!descriptor) {
        return Result<void>::failure(
            ErrorCode::io,
            "open " + path + ": " + std::strerror(errno));
    }
    auto written = write_all(descriptor.get(), record);
    if (written && ::fsync(descriptor.get()) != 0) {
        written = Result<void>::failure(
            ErrorCode::io,
            "fsync " + path + ": " + std::strerror(errno));
    }
    if (!written) {
        descriptor.reset();
        static_cast<void>(::unlink(path.c_str()));
        return written;
    }
    return Result<void>::success();
}

} // namespace xmin::next
