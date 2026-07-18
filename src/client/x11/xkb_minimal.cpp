/*
 * Minimal xkbcommon-compatible state over Xmin's canonical fixed US keymap.
 *
 * Xmin does not load or mutate an XKB rules database.  Its XKB protocol and
 * core keyboard mapping both expose core_keymap.hpp, so qxcb can use the same
 * immutable map without carrying libxkbcommon and xkeyboard-config.
 */
#include <xmin/server/generated/core_keymap.hpp>

#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

struct xkb_context {
    std::atomic<unsigned> references{1};
    xkb_log_level log_level = XKB_LOG_LEVEL_ERROR;
};

struct xkb_keymap {
    std::atomic<unsigned> references{1};
};

struct xkb_state {
    std::atomic<unsigned> references{1};
    xkb_keymap *keymap = nullptr;
    xkb_mod_mask_t depressed_mods = 0;
    xkb_mod_mask_t latched_mods = 0;
    xkb_mod_mask_t locked_mods = 0;
    xkb_layout_index_t depressed_layout = 0;
    xkb_layout_index_t latched_layout = 0;
    xkb_layout_index_t locked_layout = 0;
};

namespace {

using xmin::server::core_keymap;
using xmin::server::maximum_keycode;
using xmin::server::minimum_keycode;

constexpr std::array<std::string_view, 8> modifier_names{{
    XKB_MOD_NAME_SHIFT,
    XKB_MOD_NAME_CAPS,
    XKB_MOD_NAME_CTRL,
    XKB_MOD_NAME_MOD1,
    XKB_MOD_NAME_MOD2,
    XKB_MOD_NAME_MOD3,
    XKB_MOD_NAME_MOD4,
    XKB_MOD_NAME_MOD5,
}};

constexpr xkb_mod_mask_t modifier_bit(xkb_mod_index_t index)
{
    return index >= 32 ? 0U : xkb_mod_mask_t{1} << index;
}

bool valid_key(xkb_keycode_t key)
{
    return key >= minimum_keycode && key <= maximum_keycode &&
           core_keymap[key][0] != XKB_KEY_NoSymbol;
}

xkb_mod_mask_t effective_mods(const xkb_state *state)
{
    return state->depressed_mods | state->latched_mods | state->locked_mods;
}

bool ascii_letter(xkb_keysym_t symbol)
{
    return (symbol >= 'a' && symbol <= 'z') ||
           (symbol >= 'A' && symbol <= 'Z');
}

bool keypad_pair(xkb_keysym_t first, xkb_keysym_t second)
{
    return first >= 0xff80U && first <= 0xff9fU &&
           second >= 0xffaaU && second <= 0xffbdU;
}

xkb_level_index_t level_for_key(const xkb_state *state, xkb_keycode_t key)
{
    if (!valid_key(key))
        return XKB_LEVEL_INVALID;
    const auto &row = core_keymap[key];
    const auto mods = effective_mods(state);
    const bool shift = (mods & modifier_bit(0)) != 0;
    const bool caps = (mods & modifier_bit(1)) != 0;
    const bool num_lock = (mods & modifier_bit(4)) != 0;
    if (keypad_pair(row[0], row[1]))
        return (num_lock != shift) ? 1U : 0U;
    if (ascii_letter(row[0]))
        return (shift != caps) ? 1U : 0U;
    return shift && row[1] != XKB_KEY_NoSymbol ? 1U : 0U;
}

xkb_keysym_t symbol_for_key(const xkb_state *state, xkb_keycode_t key)
{
    const auto level = level_for_key(state, key);
    if (level == XKB_LEVEL_INVALID)
        return XKB_KEY_NoSymbol;
    const xkb_keysym_t symbol = core_keymap[key][level];
    return symbol != XKB_KEY_NoSymbol ? symbol : core_keymap[key][0];
}

std::uint32_t keysym_codepoint(xkb_keysym_t symbol)
{
    if ((symbol >= 0x20U && symbol <= 0x7eU) ||
        (symbol >= 0xa0U && symbol <= 0xffU))
        return symbol;
    if (symbol >= 0x01000100U && symbol <= 0x0110ffffU)
        return symbol & 0x00ffffffU;
    switch (symbol) {
    case XKB_KEY_BackSpace:
        return 0x08;
    case XKB_KEY_Tab:
    case XKB_KEY_KP_Tab:
        return 0x09;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        return 0x0d;
    case XKB_KEY_Escape:
        return 0x1b;
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
        return 0x7f;
    case XKB_KEY_KP_Space:
        return ' ';
    case XKB_KEY_KP_Multiply:
        return '*';
    case XKB_KEY_KP_Add:
        return '+';
    case XKB_KEY_KP_Separator:
        return ',';
    case XKB_KEY_KP_Subtract:
        return '-';
    case XKB_KEY_KP_Decimal:
        return '.';
    case XKB_KEY_KP_Divide:
        return '/';
    case XKB_KEY_KP_0:
    case XKB_KEY_KP_1:
    case XKB_KEY_KP_2:
    case XKB_KEY_KP_3:
    case XKB_KEY_KP_4:
    case XKB_KEY_KP_5:
    case XKB_KEY_KP_6:
    case XKB_KEY_KP_7:
    case XKB_KEY_KP_8:
    case XKB_KEY_KP_9:
        return '0' + (symbol - XKB_KEY_KP_0);
    default:
        return 0;
    }
}

std::size_t encode_utf8(std::uint32_t codepoint, char output[4])
{
    if (codepoint <= 0x7fU) {
        output[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (codepoint <= 0x7ffU) {
        output[0] = static_cast<char>(0xc0U | (codepoint >> 6));
        output[1] = static_cast<char>(0x80U | (codepoint & 0x3fU));
        return 2;
    }
    if (codepoint >= 0xd800U && codepoint <= 0xdfffU)
        return 0;
    if (codepoint <= 0xffffU) {
        output[0] = static_cast<char>(0xe0U | (codepoint >> 12));
        output[1] = static_cast<char>(0x80U | ((codepoint >> 6) & 0x3fU));
        output[2] = static_cast<char>(0x80U | (codepoint & 0x3fU));
        return 3;
    }
    if (codepoint <= 0x10ffffU) {
        output[0] = static_cast<char>(0xf0U | (codepoint >> 18));
        output[1] = static_cast<char>(0x80U | ((codepoint >> 12) & 0x3fU));
        output[2] = static_cast<char>(0x80U | ((codepoint >> 6) & 0x3fU));
        output[3] = static_cast<char>(0x80U | (codepoint & 0x3fU));
        return 4;
    }
    return 0;
}

int copy_text(std::string_view text, char *buffer, std::size_t size)
{
    if (buffer != nullptr && size != 0) {
        const std::size_t count = std::min(text.size(), size - 1U);
        std::memcpy(buffer, text.data(), count);
        buffer[count] = '\0';
    }
    return static_cast<int>(text.size());
}

struct NamedKeysym {
    xkb_keysym_t symbol;
    std::string_view name;
};

constexpr std::array<NamedKeysym, 44> named_keysyms{{
    {XKB_KEY_NoSymbol, "NoSymbol"}, {XKB_KEY_BackSpace, "BackSpace"},
    {XKB_KEY_Tab, "Tab"}, {XKB_KEY_Return, "Return"},
    {XKB_KEY_Escape, "Escape"}, {XKB_KEY_Delete, "Delete"},
    {XKB_KEY_Home, "Home"}, {XKB_KEY_Left, "Left"},
    {XKB_KEY_Up, "Up"}, {XKB_KEY_Right, "Right"},
    {XKB_KEY_Down, "Down"}, {XKB_KEY_Prior, "Prior"},
    {XKB_KEY_Next, "Next"}, {XKB_KEY_End, "End"},
    {XKB_KEY_Insert, "Insert"}, {XKB_KEY_Num_Lock, "Num_Lock"},
    {XKB_KEY_Shift_L, "Shift_L"}, {XKB_KEY_Shift_R, "Shift_R"},
    {XKB_KEY_Control_L, "Control_L"}, {XKB_KEY_Control_R, "Control_R"},
    {XKB_KEY_Caps_Lock, "Caps_Lock"}, {XKB_KEY_Alt_L, "Alt_L"},
    {XKB_KEY_Alt_R, "Alt_R"}, {XKB_KEY_Super_L, "Super_L"},
    {XKB_KEY_Super_R, "Super_R"}, {XKB_KEY_Menu, "Menu"},
    {XKB_KEY_KP_Enter, "KP_Enter"}, {XKB_KEY_KP_Add, "KP_Add"},
    {XKB_KEY_KP_Subtract, "KP_Subtract"},
    {XKB_KEY_KP_Multiply, "KP_Multiply"},
    {XKB_KEY_KP_Divide, "KP_Divide"},
    {XKB_KEY_KP_Decimal, "KP_Decimal"}, {XKB_KEY_F1, "F1"},
    {XKB_KEY_F2, "F2"}, {XKB_KEY_F3, "F3"}, {XKB_KEY_F4, "F4"},
    {XKB_KEY_F5, "F5"}, {XKB_KEY_F6, "F6"}, {XKB_KEY_F7, "F7"},
    {XKB_KEY_F8, "F8"}, {XKB_KEY_F9, "F9"}, {XKB_KEY_F10, "F10"},
    {XKB_KEY_F11, "F11"}, {XKB_KEY_F12, "F12"},
}};

xkb_keymap *new_keymap()
{
    return new (std::nothrow) xkb_keymap;
}

} // namespace

extern "C" {

xkb_context *xkb_context_new(xkb_context_flags)
{
    return new (std::nothrow) xkb_context;
}

void xkb_context_set_log_level(xkb_context *context, xkb_log_level level)
{
    if (context != nullptr)
        context->log_level = level;
}

void xkb_context_unref(xkb_context *context)
{
    if (context != nullptr && context->references.fetch_sub(1) == 1)
        delete context;
}

xkb_keymap *xkb_keymap_new_from_buffer(
    xkb_context *context, const char *, std::size_t, xkb_keymap_format,
    xkb_keymap_compile_flags)
{
    return context == nullptr ? nullptr : new_keymap();
}

void xkb_keymap_unref(xkb_keymap *keymap)
{
    if (keymap != nullptr && keymap->references.fetch_sub(1) == 1)
        delete keymap;
}

xkb_keycode_t xkb_keymap_min_keycode(xkb_keymap *)
{
    return minimum_keycode;
}

xkb_keycode_t xkb_keymap_max_keycode(xkb_keymap *)
{
    return maximum_keycode;
}

xkb_mod_index_t xkb_keymap_mod_get_index(xkb_keymap *, const char *name)
{
    if (name == nullptr)
        return XKB_MOD_INVALID;
    const auto match = std::find(
        modifier_names.begin(), modifier_names.end(), std::string_view{name});
    return match == modifier_names.end()
               ? XKB_MOD_INVALID
               : static_cast<xkb_mod_index_t>(match - modifier_names.begin());
}

xkb_layout_index_t xkb_keymap_num_layouts(xkb_keymap *)
{
    return 1;
}

xkb_layout_index_t xkb_keymap_num_layouts_for_key(
    xkb_keymap *, xkb_keycode_t key)
{
    return valid_key(key) ? 1U : 0U;
}

int xkb_keymap_key_get_syms_by_level(
    xkb_keymap *, xkb_keycode_t key, xkb_layout_index_t layout,
    xkb_level_index_t level, const xkb_keysym_t **symbols)
{
    if (symbols == nullptr)
        return 0;
    *symbols = nullptr;
    if (!valid_key(key) || layout != 0 || level > 1 ||
        core_keymap[key][level] == XKB_KEY_NoSymbol)
        return 0;
    *symbols = &core_keymap[key][level];
    return 1;
}

xkb_state *xkb_state_new(xkb_keymap *keymap)
{
    if (keymap == nullptr)
        return nullptr;
    auto *state = new (std::nothrow) xkb_state;
    if (state == nullptr)
        return nullptr;
    keymap->references.fetch_add(1);
    state->keymap = keymap;
    return state;
}

void xkb_state_unref(xkb_state *state)
{
    if (state == nullptr || state->references.fetch_sub(1) != 1)
        return;
    xkb_keymap_unref(state->keymap);
    delete state;
}

xkb_keymap *xkb_state_get_keymap(xkb_state *state)
{
    return state == nullptr ? nullptr : state->keymap;
}

xkb_state_component xkb_state_update_mask(
    xkb_state *state, xkb_mod_mask_t depressed_mods,
    xkb_mod_mask_t latched_mods, xkb_mod_mask_t locked_mods,
    xkb_layout_index_t, xkb_layout_index_t, xkb_layout_index_t)
{
    if (state == nullptr)
        return static_cast<xkb_state_component>(0);
    unsigned changed = 0;
    const auto old_effective = effective_mods(state);
    if (state->depressed_mods != depressed_mods)
        changed |= XKB_STATE_MODS_DEPRESSED;
    if (state->latched_mods != latched_mods)
        changed |= XKB_STATE_MODS_LATCHED;
    if (state->locked_mods != locked_mods)
        changed |= XKB_STATE_MODS_LOCKED;
    state->depressed_mods = depressed_mods;
    state->latched_mods = latched_mods;
    state->locked_mods = locked_mods;
    if (old_effective != effective_mods(state))
        changed |= XKB_STATE_MODS_EFFECTIVE;
    return static_cast<xkb_state_component>(changed);
}

xkb_layout_index_t xkb_state_key_get_layout(
    xkb_state *, xkb_keycode_t key)
{
    return valid_key(key) ? 0U : XKB_LAYOUT_INVALID;
}

xkb_level_index_t xkb_state_key_get_level(
    xkb_state *state, xkb_keycode_t key, xkb_layout_index_t layout)
{
    return state == nullptr || layout != 0 ? XKB_LEVEL_INVALID
                                           : level_for_key(state, key);
}

xkb_keysym_t xkb_state_key_get_one_sym(xkb_state *state, xkb_keycode_t key)
{
    return state == nullptr ? XKB_KEY_NoSymbol : symbol_for_key(state, key);
}

int xkb_state_key_get_utf8(
    xkb_state *state, xkb_keycode_t key, char *buffer, std::size_t size)
{
    if (state == nullptr)
        return 0;
    std::uint32_t codepoint = keysym_codepoint(symbol_for_key(state, key));
    if ((effective_mods(state) & modifier_bit(2)) != 0 && codepoint <= 0x7fU) {
        if ((codepoint >= '@' && codepoint <= '_') ||
            (codepoint >= 'a' && codepoint <= 'z'))
            codepoint &= 0x1fU;
        else if (codepoint == ' ')
            codepoint = 0;
    }
    if (codepoint == 0)
        return 0;
    char encoded[4]{};
    const std::size_t length = encode_utf8(codepoint, encoded);
    if (buffer != nullptr && size != 0) {
        const std::size_t count = std::min(length, size - 1U);
        std::memcpy(buffer, encoded, count);
        buffer[count] = '\0';
    }
    return static_cast<int>(length);
}

xkb_mod_mask_t xkb_state_serialize_mods(
    xkb_state *state, xkb_state_component components)
{
    if (state == nullptr)
        return 0;
    if ((components & XKB_STATE_MODS_EFFECTIVE) != 0)
        return effective_mods(state);
    xkb_mod_mask_t result = 0;
    if ((components & XKB_STATE_MODS_DEPRESSED) != 0)
        result |= state->depressed_mods;
    if ((components & XKB_STATE_MODS_LATCHED) != 0)
        result |= state->latched_mods;
    if ((components & XKB_STATE_MODS_LOCKED) != 0)
        result |= state->locked_mods;
    return result;
}

xkb_layout_index_t xkb_state_serialize_layout(
    xkb_state *, xkb_state_component)
{
    return 0;
}

int xkb_state_mod_name_is_active(
    xkb_state *state, const char *name, xkb_state_component components)
{
    if (state == nullptr)
        return -1;
    const auto index = xkb_keymap_mod_get_index(state->keymap, name);
    if (index == XKB_MOD_INVALID)
        return -1;
    return (xkb_state_serialize_mods(state, components) &
            modifier_bit(index)) != 0;
}

int xkb_keysym_to_utf8(
    xkb_keysym_t symbol, char *buffer, std::size_t size)
{
    const std::uint32_t codepoint = keysym_codepoint(symbol);
    if (codepoint == 0)
        return 0;
    char encoded[4]{};
    const std::size_t length = encode_utf8(codepoint, encoded);
    if (buffer != nullptr && size != 0) {
        const std::size_t count = std::min(length, size - 1U);
        std::memcpy(buffer, encoded, count);
        buffer[count] = '\0';
    }
    return static_cast<int>(length);
}

int xkb_keysym_get_name(
    xkb_keysym_t symbol, char *buffer, std::size_t size)
{
    const auto named = std::find_if(
        named_keysyms.begin(), named_keysyms.end(),
        [symbol](const NamedKeysym &entry) { return entry.symbol == symbol; });
    if (named != named_keysyms.end())
        return copy_text(named->name, buffer, size);
    if (symbol >= 0x21U && symbol <= 0x7eU) {
        const char character = static_cast<char>(symbol);
        return copy_text(std::string_view{&character, 1}, buffer, size);
    }
    char name[32]{};
    const int length = symbol >= 0x01000100U && symbol <= 0x0110ffffU
        ? std::snprintf(name, sizeof(name), "U%04X", symbol & 0x00ffffffU)
        : std::snprintf(name, sizeof(name), "0x%08X", symbol);
    if (length < 0)
        return -1;
    return copy_text(
        std::string_view{name, static_cast<std::size_t>(length)}, buffer,
        size);
}

std::int32_t xkb_x11_get_core_keyboard_device_id(xcb_connection_t *connection)
{
    return connection == nullptr || xcb_connection_has_error(connection) ? -1
                                                                          : 3;
}

int xkb_x11_setup_xkb_extension(
    xcb_connection_t *connection, std::uint16_t major,
    std::uint16_t minor, xkb_x11_setup_xkb_extension_flags,
    std::uint16_t *major_out, std::uint16_t *minor_out,
    std::uint8_t *event_out, std::uint8_t *error_out)
{
    if (connection == nullptr || xcb_connection_has_error(connection))
        return 0;
    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(connection, &xcb_xkb_id);
    if (extension == nullptr || !extension->present)
        return 0;
    xcb_xkb_use_extension_reply_t *reply = xcb_xkb_use_extension_reply(
        connection, xcb_xkb_use_extension(connection, major, minor), nullptr);
    if (reply == nullptr)
        return 0;
    const bool supported = reply->supported != 0;
    if (major_out != nullptr)
        *major_out = reply->serverMajor;
    if (minor_out != nullptr)
        *minor_out = reply->serverMinor;
    if (event_out != nullptr)
        *event_out = extension->first_event;
    if (error_out != nullptr)
        *error_out = extension->first_error;
    std::free(reply);
    return supported ? 1 : 0;
}

xkb_keymap *xkb_x11_keymap_new_from_device(
    xkb_context *context, xcb_connection_t *connection, std::int32_t device_id,
    xkb_keymap_compile_flags)
{
    return context == nullptr || connection == nullptr || device_id != 3
               ? nullptr
               : new_keymap();
}

xkb_state *xkb_x11_state_new_from_device(
    xkb_keymap *keymap, xcb_connection_t *connection, std::int32_t device_id)
{
    return connection == nullptr || device_id != 3 ? nullptr
                                                    : xkb_state_new(keymap);
}

} // extern "C"
