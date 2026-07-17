#include "xmin/server/clock.hpp"

namespace xmin::server {

Clock &
default_clock() noexcept
{
    static SteadyClock clock;
    return clock;
}

} // namespace xmin::server
