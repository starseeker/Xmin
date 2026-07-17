#ifndef XMIN_SERVER_CLOCK_HPP
#define XMIN_SERVER_CLOCK_HPP

#include <chrono>

namespace xmin::server {

class Clock {
public:
    using time_point = std::chrono::steady_clock::time_point;

    virtual ~Clock() = default;
    [[nodiscard]] virtual time_point now() const noexcept = 0;
};

class SteadyClock final : public Clock {
public:
    [[nodiscard]] time_point now() const noexcept override
    {
        return std::chrono::steady_clock::now();
    }
};

[[nodiscard]] Clock &default_clock() noexcept;

} // namespace xmin::server

#endif
