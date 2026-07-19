#include "viewer_transport.hpp"

namespace xmin::viewer {

std::unique_ptr<GuestTransport>
create_guest_transport(const std::string &, const std::string &, bool,
                       std::string &error)
{
    error = "this platform does not yet provide an Xmin viewer transport";
    return {};
}

} // namespace xmin::viewer
