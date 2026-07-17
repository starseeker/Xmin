#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <xcb/xcb.h>
#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>

int
main(void)
{
    xcb_connection_t *connection = xcb_connect(NULL, NULL);
    struct xkb_context *context =
        xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = NULL;
    struct xkb_state *state = NULL;
    xcb_generic_error_t *error = NULL;
    xcb_xkb_get_state_reply_t *wire_state = NULL;
    uint16_t major = 0;
    uint16_t minor = 0;
    uint8_t event_base = 0;
    uint8_t error_base = 0;
    uint8_t initial_locks = 0;
    xkb_keysym_t symbol = XKB_KEY_NoSymbol;
    const char *stage = "extension setup";
    int restore = 0;
    int result = 0;

    if (xcb_connection_has_error(connection) || context == NULL ||
        !xkb_x11_setup_xkb_extension(
            connection, 1, 0, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
            &major, &minor, &event_base, &error_base) ||
        major != 1 || minor != 0 || event_base == 0 || error_base == 0) {
        goto cleanup;
    }
    stage = "keymap import";
    keymap = xkb_x11_keymap_new_from_device(
        context, connection, 3, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap == NULL)
        goto cleanup;
    stage = "initial state import";
    state = xkb_x11_state_new_from_device(keymap, connection, 3);
    if (state == NULL)
        goto cleanup;
    symbol = xkb_state_key_get_one_sym(state, 38);
    if (symbol != 'a')
        goto cleanup;

    stage = "wire state query";
    wire_state = xcb_xkb_get_state_reply(
        connection, xcb_xkb_get_state(connection, 3), &error);
    if (error != NULL || wire_state == NULL)
        goto cleanup;
    initial_locks = wire_state->lockedMods;
    free(wire_state);
    wire_state = NULL;
    stage = "Caps Lock update";
    xcb_void_cookie_t cookie = xcb_xkb_latch_lock_state_checked(
        connection, 3, XCB_MOD_MASK_LOCK,
        initial_locks ^ XCB_MOD_MASK_LOCK, 0, 0, 0, 0, 0);
    error = xcb_request_check(connection, cookie);
    if (error != NULL)
        goto cleanup;
    restore = 1;
    xkb_state_unref(state);
    stage = "updated state import";
    state = xkb_x11_state_new_from_device(keymap, connection, 3);
    if (state == NULL)
        goto cleanup;
    symbol = xkb_state_key_get_one_sym(state, 38);
    if (symbol != 'A')
        goto cleanup;
    result = 1;

cleanup:
    if (restore) {
        xcb_xkb_latch_lock_state(
            connection, 3, XCB_MOD_MASK_LOCK, initial_locks,
            0, 0, 0, 0, 0);
    }
    if (!result)
        fprintf(stderr,
                "xkbcommon could not consume Xmin's XKB model at %s "
                "(keysym=%#x)\n", stage, symbol);
    free(wire_state);
    free(error);
    xkb_state_unref(state);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    xcb_disconnect(connection);
    return result ? 0 : 1;
}
