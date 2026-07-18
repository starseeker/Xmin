#include "client_connection.hpp"

#include "xmin/server/xauthority.hpp"

#include <xcb/bigreq.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace xmin::client::x11 {
namespace {

constexpr std::string_view authentication_protocol = "MIT-MAGIC-COOKIE-1";
constexpr std::size_t maximum_packet_size = 64U * 1024U * 1024U;

bool
host_is_little_endian() noexcept
{
    const std::uint16_t value = 1;
    std::uint8_t first = 0;
    std::memcpy(&first, &value, 1);
    return first == 1;
}

void
put16(std::uint8_t *bytes, std::uint16_t value) noexcept
{
    std::memcpy(bytes, &value, sizeof(value));
}

void
put32(std::uint8_t *bytes, std::uint32_t value) noexcept
{
    std::memcpy(bytes, &value, sizeof(value));
}

std::uint16_t
get16(const std::uint8_t *bytes) noexcept
{
    std::uint16_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

std::uint32_t
get32(const std::uint8_t *bytes) noexcept
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool
write_all(int descriptor, const std::uint8_t *bytes, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::send(
            descriptor, bytes + offset, size - offset, MSG_NOSIGNAL);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool
read_all(int descriptor, std::uint8_t *bytes, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::recv(
            descriptor, bytes + offset, size - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

std::optional<std::pair<unsigned, unsigned>>
parse_display(const char *display_name)
{
    if (display_name == nullptr || *display_name == '\0')
        display_name = std::getenv("DISPLAY");
    if (display_name == nullptr)
        return std::nullopt;
    std::string_view text(display_name);
    if (text.rfind("unix:", 0) == 0)
        text.remove_prefix(4);
    if (text.empty() || text.front() != ':')
        return std::nullopt;
    text.remove_prefix(1);
    const auto dot = text.find('.');
    const auto display_text = text.substr(0, dot);
    const auto screen_text = dot == std::string_view::npos
        ? std::string_view{}
        : text.substr(dot + 1);
    auto parse_number = [](std::string_view value)
        -> std::optional<unsigned> {
        if (value.empty())
            return std::nullopt;
        unsigned result = 0;
        for (const char character : value) {
            if (character < '0' || character > '9')
                return std::nullopt;
            if (result > (65535U - static_cast<unsigned>(character - '0')) / 10U)
                return std::nullopt;
            result = result * 10U + static_cast<unsigned>(character - '0');
        }
        return result;
    };
    const auto display = parse_number(display_text);
    const auto screen = screen_text.empty()
        ? std::optional<unsigned>{0}
        : parse_number(screen_text);
    if (!display || !screen)
        return std::nullopt;
    return std::pair<unsigned, unsigned>{*display, *screen};
}

std::vector<std::uint8_t>
authorization_bytes(const xcb_auth_info_t *authorization)
{
    std::vector<std::uint8_t> bytes;
    if (authorization == nullptr || authorization->namelen <= 0 ||
        authorization->datalen < 0) {
        return bytes;
    }
    bytes.insert(bytes.end(), authorization->name,
                 authorization->name + authorization->namelen);
    while ((bytes.size() & 3U) != 0)
        bytes.push_back(0);
    bytes.insert(bytes.end(), authorization->data,
                 authorization->data + authorization->datalen);
    while ((bytes.size() & 3U) != 0)
        bytes.push_back(0);
    return bytes;
}

} // namespace

Connection::Connection(int error_code)
    : error_code_(error_code)
{
}

Connection::~Connection()
{
    stopping_.store(true);
    if (socket_)
        ::shutdown(socket_.get(), SHUT_RDWR);
    state_changed_.notify_all();
    if (reader_.joinable())
        reader_.join();
    for (auto &[sequence, packet] : replies_) {
        (void) sequence;
        for (const int descriptor : packet.descriptors)
            ::close(descriptor);
    }
}

std::unique_ptr<Connection>
Connection::connect(const char *display_name, int *screen_number)
{
    auto connection = std::unique_ptr<Connection>(new Connection);
    if (!connection->open_display(display_name, screen_number))
        return connection;

    std::vector<std::uint8_t> cookie;
    std::string authority_path;
    if (const char *authority = std::getenv("XAUTHORITY");
        authority != nullptr && *authority != '\0') {
        authority_path = authority;
    }
    else if (const char *home = std::getenv("HOME");
             home != nullptr && *home != '\0') {
        authority_path = std::string(home) + "/.Xauthority";
    }
    xcb_auth_info_t authorization{};
    std::string name(authentication_protocol);
    if (!authority_path.empty()) {
        const auto display = parse_display(display_name);
        if (display) {
            auto loaded = server::load_xauthority_cookie(
                authority_path, display->first);
            if (loaded) {
                cookie = std::move(loaded.value());
                authorization.namelen = static_cast<int>(name.size());
                authorization.name = name.data();
                authorization.datalen = static_cast<int>(cookie.size());
                authorization.data = reinterpret_cast<char *>(cookie.data());
            }
        }
    }
    if (!connection->perform_setup(
            authorization.namelen == 0 ? nullptr : &authorization)) {
        connection->socket_.reset();
        return connection;
    }
    connection->error_code_.store(0);
    connection->start_reader();
    return connection;
}

std::unique_ptr<Connection>
Connection::connect_to_fd(int descriptor, const xcb_auth_info_t *authorization)
{
    auto connection = std::unique_ptr<Connection>(new Connection);
    connection->socket_.reset(descriptor);
    if (!connection->perform_setup(authorization)) {
        connection->socket_.reset();
        return connection;
    }
    connection->error_code_.store(0);
    connection->start_reader();
    return connection;
}

bool
Connection::open_display(const char *display_name, int *screen_number)
{
    const auto parsed = parse_display(display_name);
    if (!parsed) {
        error_code_.store(XCB_CONN_CLOSED_PARSE_ERR);
        return false;
    }
    if (screen_number != nullptr)
        *screen_number = static_cast<int>(parsed->second);
    if (parsed->second != 0) {
        error_code_.store(XCB_CONN_CLOSED_INVALID_SCREEN);
        return false;
    }
    const std::string path =
        "/tmp/.X11-unix/X" + std::to_string(parsed->first);
    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path)) {
        error_code_.store(XCB_CONN_CLOSED_PARSE_ERR);
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||        \
    defined(__OpenBSD__)
    address.sun_len = static_cast<std::uint8_t>(address_size);
#endif
    socket_.reset(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!socket_ || ::connect(
            socket_.get(), reinterpret_cast<const sockaddr *>(&address),
            address_size) != 0) {
        error_code_.store(XCB_CONN_ERROR);
        socket_.reset();
        return false;
    }
    return true;
}

bool
Connection::perform_setup(const xcb_auth_info_t *authorization)
{
    const std::uint16_t name_size = authorization == nullptr
        ? 0
        : static_cast<std::uint16_t>(authorization->namelen);
    const std::uint16_t data_size = authorization == nullptr
        ? 0
        : static_cast<std::uint16_t>(authorization->datalen);
    std::array<std::uint8_t, 12> prefix{};
    prefix[0] = host_is_little_endian() ? 'l' : 'B';
    put16(prefix.data() + 2, 11);
    put16(prefix.data() + 6, name_size);
    put16(prefix.data() + 8, data_size);
    const auto authentication = authorization_bytes(authorization);
    if (!write_all(socket_.get(), prefix.data(), prefix.size()) ||
        (!authentication.empty() && !write_all(
            socket_.get(), authentication.data(), authentication.size()))) {
        error_code_.store(XCB_CONN_ERROR);
        return false;
    }

    std::array<std::uint8_t, 8> reply_prefix{};
    if (!read_all(socket_.get(), reply_prefix.data(), reply_prefix.size())) {
        error_code_.store(XCB_CONN_ERROR);
        return false;
    }
    const std::size_t body_size =
        static_cast<std::size_t>(get16(reply_prefix.data() + 6)) * 4U;
    if (body_size > maximum_packet_size) {
        error_code_.store(XCB_CONN_CLOSED_MEM_INSUFFICIENT);
        return false;
    }
    setup_bytes_.resize(reply_prefix.size() + body_size);
    std::memcpy(setup_bytes_.data(), reply_prefix.data(), reply_prefix.size());
    if (body_size != 0 && !read_all(
            socket_.get(), setup_bytes_.data() + reply_prefix.size(),
            body_size)) {
        error_code_.store(XCB_CONN_ERROR);
        return false;
    }
    if (reply_prefix[0] != 1 || setup_bytes_.size() < sizeof(xcb_setup_t)) {
        error_code_.store(XCB_CONN_ERROR);
        return false;
    }
    const auto *decoded = reinterpret_cast<const xcb_setup_t *>(
        setup_bytes_.data());
    if (decoded->roots_len == 0) {
        error_code_.store(XCB_CONN_CLOSED_INVALID_SCREEN);
        return false;
    }
    maximum_request_length_.store(decoded->maximum_request_length);
    return true;
}

void
Connection::start_reader()
{
    reader_ = std::thread([this] { reader_loop(); });
}

void
Connection::reader_loop()
{
    while (!stopping_.load()) {
        Packet packet;
        if (!read_packet(packet))
            break;
        queue_packet(std::move(packet));
    }
    if (!stopping_.load())
        error_code_.store(XCB_CONN_ERROR);
    state_changed_.notify_all();
}

bool
Connection::read_packet(Packet &packet)
{
    std::array<std::uint8_t, 32> header{};
    std::array<std::uint8_t, CMSG_SPACE(sizeof(int) * 8)> control{};
    iovec vector{header.data(), header.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    ssize_t count;
    do {
        count = ::recvmsg(socket_.get(), &message, MSG_WAITALL);
    } while (count < 0 && errno == EINTR);
    if (count != static_cast<ssize_t>(header.size()))
        return false;
    for (cmsghdr *item = CMSG_FIRSTHDR(&message); item != nullptr;
         item = CMSG_NXTHDR(&message, item)) {
        if (item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS)
            continue;
        const std::size_t bytes = item->cmsg_len - CMSG_LEN(0);
        const auto descriptor_count = bytes / sizeof(int);
        const auto *descriptors = reinterpret_cast<const int *>(
            CMSG_DATA(item));
        packet.descriptors.insert(
            packet.descriptors.end(), descriptors,
            descriptors + descriptor_count);
    }

    const std::uint8_t type = header[0] & 0x7fU;
    std::size_t extra = 0;
    if (type == 1 || type == XCB_GE_GENERIC)
        extra = static_cast<std::size_t>(get32(header.data() + 4)) * 4U;
    if (extra > maximum_packet_size - header.size())
        return false;
    packet.bytes.resize(header.size() + extra);
    std::memcpy(packet.bytes.data(), header.data(), header.size());
    if (extra != 0 && !read_all(
            socket_.get(), packet.bytes.data() + header.size(), extra)) {
        return false;
    }
    packet.sequence = widen_sequence(get16(header.data() + 2));
    return true;
}

void
Connection::queue_packet(Packet packet)
{
    const std::uint8_t type = packet.bytes[0] & 0x7fU;
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (type == 1) {
        if (discarded_replies_.erase(packet.sequence) != 0) {
            for (const int descriptor : packet.descriptors)
                ::close(descriptor);
        }
        else {
            replies_[packet.sequence] = std::move(packet);
        }
        reply_requests_.erase(packet.sequence);
    }
    else if (type == 0) {
        if (checked_requests_.count(packet.sequence) != 0 ||
            reply_requests_.count(packet.sequence) != 0) {
            errors_[packet.sequence] = std::move(packet);
        }
        else {
            events_.push_back(std::move(packet));
        }
        reply_requests_.erase(packet.sequence);
    }
    else {
        events_.push_back(std::move(packet));
    }
    state_changed_.notify_all();
}

std::uint64_t
Connection::widen_sequence(std::uint16_t wire_sequence) const noexcept
{
    const std::uint64_t sent = sequence_.load();
    std::uint64_t candidate =
        (sent & ~std::uint64_t{0xffff}) | wire_sequence;
    if (candidate > sent)
        candidate -= std::uint64_t{1} << 16;
    return candidate;
}

std::uint64_t
Connection::send(
    int flags, const struct iovec *vectors,
    const xcb_protocol_request_t &request,
    const int *descriptors, std::size_t descriptor_count)
{
    if (error() != 0 || request.count == 0)
        return 0;
    std::vector<std::uint8_t> bytes;
    try {
        std::size_t size = 0;
        for (std::size_t index = 0; index < request.count; ++index) {
            if (vectors[index].iov_len > maximum_packet_size - size)
                return 0;
            size += vectors[index].iov_len;
        }
        if (size < 4)
            return 0;
        bytes.reserve(size + 4);
        for (std::size_t index = 0; index < request.count; ++index) {
            const auto length = vectors[index].iov_len;
            if (vectors[index].iov_base == nullptr) {
                bytes.insert(bytes.end(), length, 0);
            }
            else {
                const auto *first = static_cast<const std::uint8_t *>(
                    vectors[index].iov_base);
                bytes.insert(bytes.end(), first, first + length);
            }
        }
        if (request.ext != nullptr) {
            const auto *data = extension(request.ext);
            if (data == nullptr || data->present == 0) {
                error_code_.store(XCB_CONN_CLOSED_EXT_NOTSUPPORTED);
                return 0;
            }
            bytes[0] = data->major_opcode;
            bytes[1] = request.opcode;
        }
        else {
            bytes[0] = request.opcode;
        }
        const std::size_t words = (bytes.size() + 3U) / 4U;
        bytes.resize(words * 4U, 0);
        if (words <= std::numeric_limits<std::uint16_t>::max()) {
            put16(bytes.data() + 2, static_cast<std::uint16_t>(words));
        }
        else {
            if (maximum_request_length() <= words + 1U)
                return 0;
            bytes.insert(bytes.begin() + 4, 4, 0);
            put16(bytes.data() + 2, 0);
            put32(bytes.data() + 4, static_cast<std::uint32_t>(words + 1U));
        }
    }
    catch (const std::bad_alloc &) {
        error_code_.store(XCB_CONN_CLOSED_MEM_INSUFFICIENT);
        return 0;
    }
    return send_bytes(
        std::move(bytes), flags, request.isvoid == 0,
        descriptors, descriptor_count);
}

std::uint64_t
Connection::send_bytes(
    std::vector<std::uint8_t> bytes, int flags, bool expects_reply,
    const int *descriptors, std::size_t descriptor_count)
{
    std::lock_guard<std::mutex> send_lock(send_mutex_);
    const std::uint64_t sequence = sequence_.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if ((flags & XCB_REQUEST_CHECKED) != 0)
            checked_requests_.insert(sequence);
        if (expects_reply)
            reply_requests_.insert(sequence);
        if ((flags & XCB_REQUEST_DISCARD_REPLY) != 0)
            discarded_replies_.insert(sequence);
    }
    bool written = false;
    if (descriptor_count == 0) {
        written = write_all(socket_.get(), bytes.data(), bytes.size());
    }
    else {
        std::vector<std::uint8_t> control(CMSG_SPACE(
            sizeof(int) * descriptor_count));
        iovec vector{bytes.data(), bytes.size()};
        msghdr message{};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        auto *item = CMSG_FIRSTHDR(&message);
        item->cmsg_level = SOL_SOCKET;
        item->cmsg_type = SCM_RIGHTS;
        item->cmsg_len = CMSG_LEN(sizeof(int) * descriptor_count);
        std::memcpy(CMSG_DATA(item), descriptors,
                    sizeof(int) * descriptor_count);
        ssize_t count;
        do {
            count = ::sendmsg(socket_.get(), &message, MSG_NOSIGNAL);
        } while (count < 0 && errno == EINTR);
        written = count == static_cast<ssize_t>(bytes.size());
        for (std::size_t index = 0; index < descriptor_count; ++index)
            ::close(descriptors[index]);
    }
    if (!written) {
        error_code_.store(XCB_CONN_ERROR);
        state_changed_.notify_all();
        return 0;
    }
    return sequence;
}

std::uint64_t
Connection::send_core_reply_request(
    std::uint8_t opcode, std::uint8_t data,
    std::vector<std::uint8_t> body)
{
    std::vector<std::uint8_t> bytes(4 + body.size());
    bytes[0] = opcode;
    bytes[1] = data;
    put16(bytes.data() + 2,
          static_cast<std::uint16_t>((bytes.size() + 3U) / 4U));
    std::copy(body.begin(), body.end(), bytes.begin() + 4);
    bytes.resize((bytes.size() + 3U) & ~std::size_t{3}, 0);
    return send_bytes(std::move(bytes), XCB_REQUEST_CHECKED, true, nullptr, 0);
}

void *
Connection::wait_for_reply(
    std::uint64_t sequence, xcb_generic_error_t **reported_error)
{
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_changed_.wait(lock, [this, sequence] {
        return replies_.count(sequence) != 0 || errors_.count(sequence) != 0 ||
            error() != 0 || stopping_.load();
    });
    if (auto found = errors_.find(sequence); found != errors_.end()) {
        Packet packet = std::move(found->second);
        errors_.erase(found);
        checked_requests_.erase(sequence);
        lock.unlock();
        if (reported_error != nullptr)
            *reported_error = allocate_error(std::move(packet));
        else {
            std::lock_guard<std::mutex> relock(state_mutex_);
            events_.push_back(std::move(packet));
        }
        return nullptr;
    }
    auto found = replies_.find(sequence);
    if (found == replies_.end())
        return nullptr;
    Packet packet = std::move(found->second);
    replies_.erase(found);
    checked_requests_.erase(sequence);
    lock.unlock();
    return allocate_reply(std::move(packet));
}

bool
Connection::poll_for_reply(
    std::uint64_t sequence, void **reply, xcb_generic_error_t **reported_error)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (auto found = errors_.find(sequence); found != errors_.end()) {
        Packet packet = std::move(found->second);
        errors_.erase(found);
        checked_requests_.erase(sequence);
        if (reported_error != nullptr)
            *reported_error = allocate_error(std::move(packet));
        *reply = nullptr;
        return true;
    }
    auto found = replies_.find(sequence);
    if (found == replies_.end())
        return false;
    Packet packet = std::move(found->second);
    replies_.erase(found);
    checked_requests_.erase(sequence);
    *reply = allocate_reply(std::move(packet));
    return true;
}

xcb_generic_event_t *
Connection::wait_for_event()
{
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_changed_.wait(lock, [this] {
        return !events_.empty() || error() != 0 || stopping_.load();
    });
    if (events_.empty())
        return nullptr;
    Packet packet = std::move(events_.front());
    events_.pop_front();
    lock.unlock();
    return allocate_event(std::move(packet));
}

xcb_generic_event_t *
Connection::poll_for_event()
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (events_.empty())
        return nullptr;
    Packet packet = std::move(events_.front());
    events_.pop_front();
    return allocate_event(std::move(packet));
}

xcb_generic_error_t *
Connection::request_check(std::uint64_t target)
{
    const auto sync = send_core_reply_request(XCB_GET_INPUT_FOCUS, 0, {});
    if (sync == 0)
        return nullptr;
    std::free(wait_for_reply(sync, nullptr));
    std::lock_guard<std::mutex> lock(state_mutex_);
    checked_requests_.erase(target);
    auto found = errors_.find(target);
    if (found == errors_.end())
        return nullptr;
    Packet packet = std::move(found->second);
    errors_.erase(found);
    return allocate_error(std::move(packet));
}

void
Connection::discard_reply(std::uint64_t sequence)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (auto found = replies_.find(sequence); found != replies_.end()) {
        for (const int descriptor : found->second.descriptors)
            ::close(descriptor);
        replies_.erase(found);
    }
    else {
        discarded_replies_.insert(sequence);
    }
    errors_.erase(sequence);
    reply_requests_.erase(sequence);
    checked_requests_.erase(sequence);
}

const xcb_query_extension_reply_t *
Connection::extension(xcb_extension_t *requested)
{
    if (requested == nullptr || requested->name == nullptr)
        return nullptr;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto found = extensions_.find(requested->name);
        if (found != extensions_.end())
            return &found->second;
    }
    const std::string_view name(requested->name);
    std::vector<std::uint8_t> body(
        4 + ((name.size() + 3U) & ~std::size_t{3}), 0);
    put16(body.data(), static_cast<std::uint16_t>(name.size()));
    std::memcpy(body.data() + 4, name.data(), name.size());
    const auto sequence = send_core_reply_request(
        XCB_QUERY_EXTENSION, 0, std::move(body));
    auto *raw = static_cast<xcb_query_extension_reply_t *>(
        wait_for_reply(sequence, nullptr));
    xcb_query_extension_reply_t reply{};
    if (raw != nullptr) {
        reply = *raw;
        std::free(raw);
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto inserted = extensions_.emplace(requested->name, reply);
    return &inserted.first->second;
}

int
Connection::error() const noexcept
{
    return error_code_.load();
}

int
Connection::descriptor() const noexcept
{
    return socket_ ? socket_.get() : -1;
}

const xcb_setup_t *
Connection::setup() const noexcept
{
    return setup_bytes_.size() >= sizeof(xcb_setup_t)
        ? reinterpret_cast<const xcb_setup_t *>(setup_bytes_.data())
        : nullptr;
}

std::uint32_t
Connection::maximum_request_length()
{
    return maximum_request_length_.load();
}

std::uint32_t
Connection::generate_id() noexcept
{
    const auto *value = setup();
    if (value == nullptr)
        return UINT32_MAX;
    const std::uint32_t next = next_resource_.fetch_add(1);
    if ((next & ~value->resource_id_mask) != 0)
        return UINT32_MAX;
    return value->resource_id_base | (next & value->resource_id_mask);
}

void *
Connection::allocate_reply(Packet packet)
{
    const std::size_t descriptor_bytes =
        packet.descriptors.size() * sizeof(int);
    auto *result = static_cast<std::uint8_t *>(
        std::malloc(packet.bytes.size() + descriptor_bytes));
    if (result == nullptr) {
        for (const int descriptor : packet.descriptors)
            ::close(descriptor);
        return nullptr;
    }
    std::memcpy(result, packet.bytes.data(), packet.bytes.size());
    if (descriptor_bytes != 0) {
        std::memcpy(result + packet.bytes.size(), packet.descriptors.data(),
                    descriptor_bytes);
    }
    return result;
}

xcb_generic_event_t *
Connection::allocate_event(Packet packet)
{
    if ((packet.bytes[0] & 0x7fU) == 0)
        return reinterpret_cast<xcb_generic_event_t *>(
            allocate_error(std::move(packet)));
    const bool generic = (packet.bytes[0] & 0x7fU) == XCB_GE_GENERIC;
    const std::size_t wire_header = 32;
    const std::size_t extra = packet.bytes.size() - wire_header;
    auto *result = static_cast<std::uint8_t *>(
        std::calloc(1, wire_header + sizeof(std::uint32_t) + extra));
    if (result == nullptr)
        return nullptr;
    std::memcpy(result, packet.bytes.data(), wire_header);
    put32(result + wire_header, static_cast<std::uint32_t>(packet.sequence));
    if (generic && extra != 0) {
        std::memcpy(result + wire_header + sizeof(std::uint32_t),
                    packet.bytes.data() + wire_header, extra);
    }
    return reinterpret_cast<xcb_generic_event_t *>(result);
}

xcb_generic_error_t *
Connection::allocate_error(Packet packet)
{
    auto *result = static_cast<xcb_generic_error_t *>(
        std::calloc(1, sizeof(xcb_generic_error_t)));
    if (result == nullptr)
        return nullptr;
    std::memcpy(result, packet.bytes.data(),
                std::min<std::size_t>(32, packet.bytes.size()));
    result->full_sequence = static_cast<std::uint32_t>(packet.sequence);
    return result;
}

} // namespace xmin::client::x11
