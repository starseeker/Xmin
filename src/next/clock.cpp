#include "xmin/next/clock.hpp"

namespace xmin::next {

Clock &
default_clock() noexcept
{
    static SteadyClock clock;
    return clock;
}

} // namespace xmin::next
