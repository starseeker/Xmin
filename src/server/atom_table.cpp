#include "xmin/server/atom_table.hpp"

#include <array>

namespace xmin::server {
namespace {

constexpr std::array<std::string_view, 68> predefined_atoms = {
    "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL",
    "COLORMAP", "CURSOR", "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2",
    "CUT_BUFFER3", "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6",
    "CUT_BUFFER7", "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT",
    "RECTANGLE", "RESOURCE_MANAGER", "RGB_COLOR_MAP", "RGB_BEST_MAP",
    "RGB_BLUE_MAP", "RGB_DEFAULT_MAP", "RGB_GRAY_MAP", "RGB_GREEN_MAP",
    "RGB_RED_MAP", "STRING", "VISUALID", "WINDOW", "WM_COMMAND",
    "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME", "WM_ICON_SIZE",
    "WM_NAME", "WM_NORMAL_HINTS", "WM_SIZE_HINTS", "WM_ZOOM_HINTS",
    "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE",
    "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y",
    "UNDERLINE_POSITION", "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT",
    "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT", "QUAD_WIDTH",
    "WEIGHT", "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE",
    "FONT_NAME", "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT", "WM_CLASS",
    "WM_TRANSIENT_FOR",
};

} // namespace

AtomTable::AtomTable()
{
    names_.reserve(predefined_atoms.size() + 1);
    ids_.reserve(predefined_atoms.size());
    for (const auto atom : predefined_atoms)
        static_cast<void>(intern(atom));
}

AtomId
AtomTable::intern(std::string_view name, bool only_if_exists)
{
    const std::string owned_name(name);
    const auto found = ids_.find(owned_name);
    if (found != ids_.end())
        return found->second;
    if (only_if_exists)
        return 0;
    if (size() >= maximum_atoms)
        return 0;

    const auto id = static_cast<AtomId>(names_.size());
    names_.push_back(owned_name);
    ids_.emplace(names_.back(), id);
    return id;
}

std::optional<std::string_view>
AtomTable::name(AtomId atom) const
{
    if (atom == 0 || atom >= names_.size())
        return std::nullopt;
    return names_[atom];
}

} // namespace xmin::server
