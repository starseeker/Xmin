#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <xcb/xcb.h>
#include <xcb/xkb.h>

static int
checked(xcb_connection_t *connection, xcb_void_cookie_t cookie,
        const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (error == NULL)
        return 1;
    fprintf(stderr, "%s failed with X11 error %u\n",
            operation, error->error_code);
    free(error);
    return 0;
}

static xcb_generic_event_t *
next_event(xcb_connection_t *connection)
{
    xcb_generic_event_t *event = xcb_poll_for_event(connection);
    if (event != NULL)
        return event;
    xcb_flush(connection);
    struct pollfd descriptor = {
        .fd = xcb_get_file_descriptor(connection),
        .events = POLLIN
    };
    if (poll(&descriptor, 1, 2000) <= 0)
        return NULL;
    return xcb_poll_for_event(connection);
}

static int
map_has_a(xcb_xkb_get_map_reply_t *reply)
{
    xcb_xkb_get_map_map_t map;
    xcb_xkb_get_map_map_unpack(
        xcb_xkb_get_map_map(reply), reply->nTypes, reply->nKeySyms,
        reply->nKeyActions, reply->totalActions,
        reply->totalKeyBehaviors, reply->virtualMods,
        reply->totalKeyExplicit, reply->totalModMapKeys,
        reply->totalVModMapKeys, reply->present, &map);
    xcb_xkb_key_sym_map_iterator_t iterator =
        xcb_xkb_get_map_map_syms_rtrn_iterator(reply, &map);
    for (uint8_t key = reply->firstKeySym;
         iterator.rem != 0; ++key) {
        if (key == 38 && iterator.data->nSyms != 0) {
            xcb_keysym_t *symbols =
                xcb_xkb_key_sym_map_syms(iterator.data);
            return symbols[0] == 'a';
        }
        xcb_xkb_key_sym_map_next(&iterator);
    }
    return 0;
}

int
main(void)
{
    xcb_connection_t *connection = xcb_connect(NULL, NULL);
    xcb_generic_error_t *error = NULL;
    const xcb_query_extension_reply_t *extension = NULL;
    xcb_xkb_use_extension_reply_t *version = NULL;
    xcb_xkb_get_state_reply_t *state = NULL;
    xcb_xkb_get_controls_reply_t *controls = NULL;
    xcb_xkb_get_map_reply_t *map = NULL;
    xcb_xkb_get_names_reply_t *names = NULL;
    xcb_xkb_per_client_flags_reply_t *flags = NULL;
    xcb_generic_event_t *event = NULL;
    uint8_t initial_locks = 0;
    uint8_t changed_locks = 0;
    int restore_locks = 0;
    int result = 0;

    if (xcb_connection_has_error(connection))
        goto cleanup;
    extension = xcb_get_extension_data(connection, &xcb_xkb_id);
    if (extension == NULL || !extension->present)
        goto cleanup;
    version = xcb_xkb_use_extension_reply(
        connection, xcb_xkb_use_extension(connection, 1, 0), &error);
    if (error != NULL || version == NULL || !version->supported ||
        version->serverMajor != 1 || version->serverMinor != 0)
        goto cleanup;

    state = xcb_xkb_get_state_reply(
        connection, xcb_xkb_get_state(connection, 3), &error);
    if (error != NULL || state == NULL || state->deviceID == 0)
        goto cleanup;
    initial_locks = state->lockedMods;
    changed_locks = initial_locks ^ XCB_MOD_MASK_LOCK;
    free(state);
    state = NULL;

    xcb_xkb_select_events_details_t details = { 0 };
    details.affectState = 0x3fff;
    details.stateDetails = 0x3fff;
    if (!checked(connection,
                 xcb_xkb_select_events_aux_checked(
                     connection, 3, XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
                     0, 0, 0, 0, &details),
                 "XKB SelectEvents") ||
        !checked(connection,
                 xcb_xkb_latch_lock_state_checked(
                     connection, 3, XCB_MOD_MASK_LOCK, changed_locks,
                     0, 0, 0, 0, 0),
                 "XKB LatchLockState")) {
        goto cleanup;
    }
    restore_locks = 1;
    event = next_event(connection);
    if (event == NULL || (event->response_type & 0x7fU) !=
            extension->first_event ||
        ((xcb_xkb_state_notify_event_t *) event)->xkbType !=
            XCB_XKB_STATE_NOTIFY ||
        (((xcb_xkb_state_notify_event_t *) event)->lockedMods &
         XCB_MOD_MASK_LOCK) != (changed_locks & XCB_MOD_MASK_LOCK)) {
        goto cleanup;
    }
    free(event);
    event = NULL;
    state = xcb_xkb_get_state_reply(
        connection, xcb_xkb_get_state(connection, 3), &error);
    if (error != NULL || state == NULL ||
        (state->lockedMods & XCB_MOD_MASK_LOCK) !=
            (changed_locks & XCB_MOD_MASK_LOCK) ||
        (state->mods & XCB_MOD_MASK_LOCK) !=
            (changed_locks & XCB_MOD_MASK_LOCK)) {
        goto cleanup;
    }

    controls = xcb_xkb_get_controls_reply(
        connection, xcb_xkb_get_controls(connection, 3), &error);
    if (error != NULL || controls == NULL || controls->numGroups != 1 ||
        controls->repeatDelay == 0 || controls->repeatInterval == 0)
        goto cleanup;

    const uint16_t map_parts =
        XCB_XKB_MAP_PART_KEY_TYPES |
        XCB_XKB_MAP_PART_KEY_SYMS |
        XCB_XKB_MAP_PART_MODIFIER_MAP |
        XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS |
        XCB_XKB_MAP_PART_KEY_ACTIONS |
        XCB_XKB_MAP_PART_VIRTUAL_MODS |
        XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP;
    map = xcb_xkb_get_map_reply(
        connection,
        xcb_xkb_get_map(connection, 3, map_parts, 0,
                        0, 0, 0, 0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0),
        &error);
    if (error != NULL || map == NULL ||
        (map->present & map_parts) != map_parts ||
        map->minKeyCode != 8 || map->maxKeyCode != 255 ||
        map->firstType != 0 || map->nTypes == 0 ||
        map->firstKeySym != 8 || map->nKeySyms != 248 ||
        map->totalModMapKeys == 0 || !map_has_a(map)) {
        goto cleanup;
    }

    const uint32_t name_parts =
        XCB_XKB_NAME_DETAIL_KEY_TYPE_NAMES |
        XCB_XKB_NAME_DETAIL_KT_LEVEL_NAMES |
        XCB_XKB_NAME_DETAIL_KEY_NAMES |
        XCB_XKB_NAME_DETAIL_VIRTUAL_MOD_NAMES;
    names = xcb_xkb_get_names_reply(
        connection, xcb_xkb_get_names(connection, 3, name_parts), &error);
    if (error != NULL || names == NULL ||
        (names->which & name_parts) != name_parts ||
        names->nTypes == 0 || names->firstKey != 8 ||
        names->nKeys != 248) {
        goto cleanup;
    }

    flags = xcb_xkb_per_client_flags_reply(
        connection,
        xcb_xkb_per_client_flags(connection, 3,
                                 XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
                                 XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT,
                                 0, 0, 0),
        &error);
    if (error != NULL || flags == NULL ||
        (flags->supported &
         XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT) == 0 ||
        (flags->value &
         XCB_XKB_PER_CLIENT_FLAG_DETECTABLE_AUTO_REPEAT) == 0) {
        goto cleanup;
    }
    result = 1;

cleanup:
    if (restore_locks) {
        xcb_xkb_latch_lock_state(
            connection, 3, XCB_MOD_MASK_LOCK, initial_locks,
            0, 0, 0, 0, 0);
    }
    if (!result)
        fprintf(stderr, "XKB state/map contract failed\n");
    free(event);
    free(flags);
    free(names);
    free(map);
    free(controls);
    free(state);
    free(version);
    free(error);
    xcb_disconnect(connection);
    return result ? 0 : 1;
}
