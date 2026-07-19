#include <xcb/damage.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int
checked(xcb_connection_t *connection, xcb_void_cookie_t cookie,
        const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (error == NULL && xcb_connection_has_error(connection) == 0)
        return 1;
    fprintf(stderr, "%s failed with X11 error %u\n", operation,
            error == NULL ? 0U : error->error_code);
    free(error);
    return 0;
}

static int
checked_error(xcb_connection_t *connection, xcb_void_cookie_t cookie,
              uint8_t expected, const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    int result = error != NULL && error->error_code == expected;
    if (!result) {
        fprintf(stderr, "%s returned X11 error %u instead of %u\n",
                operation, error == NULL ? 0U : error->error_code,
                expected);
    }
    free(error);
    return result;
}

static xcb_damage_notify_event_t *
wait_for_damage(xcb_connection_t *connection, uint8_t first_event,
                xcb_drawable_t drawable, xcb_damage_damage_t damage)
{
    for (;;) {
        xcb_generic_event_t *event = xcb_wait_for_event(connection);
        if (event == NULL)
            return NULL;
        if ((event->response_type & 0x7fU) == first_event) {
            xcb_damage_notify_event_t *notify =
                (xcb_damage_notify_event_t *) event;
            if (notify->drawable == drawable && notify->damage == damage)
                return notify;
        }
        free(event);
    }
}

static int
region_is(xcb_connection_t *connection, xcb_xfixes_region_t region,
          const xcb_rectangle_t *expected)
{
    xcb_generic_error_t *error = NULL;
    xcb_xfixes_fetch_region_reply_t *reply =
        xcb_xfixes_fetch_region_reply(
            connection, xcb_xfixes_fetch_region(connection, region),
            &error);
    int result = error == NULL && reply != NULL &&
        xcb_xfixes_fetch_region_rectangles_length(reply) == 1;
    if (result) {
        const xcb_rectangle_t *actual =
            xcb_xfixes_fetch_region_rectangles(reply);
        result = actual[0].x == expected->x &&
            actual[0].y == expected->y &&
            actual[0].width == expected->width &&
            actual[0].height == expected->height;
    }
    free(error);
    free(reply);
    return result;
}

static int64_t
monotonic_milliseconds(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
        return -1;
    return (int64_t) time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static int
wait_for_damage_count(xcb_connection_t *connection, uint8_t first_event,
                      xcb_drawable_t drawable, xcb_damage_damage_t damage,
                      unsigned expected, int timeout_milliseconds)
{
    const int64_t started = monotonic_milliseconds();
    unsigned count = 0;
    if (started < 0)
        return 0;
    while (count < expected) {
        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(connection)) != NULL) {
            if ((event->response_type & 0x7fU) == first_event) {
                const xcb_damage_notify_event_t *notify =
                    (const xcb_damage_notify_event_t *) event;
                if (notify->drawable == drawable &&
                    notify->damage == damage) {
                    if ((notify->level & 0x7fU) !=
                            XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES ||
                        notify->area.x != 0 || notify->area.y != 0 ||
                        notify->area.width != 30 ||
                        notify->area.height != 20) {
                        fprintf(stderr,
                                "unexpected raw DAMAGE event %u: level=%u "
                                "area=%d,%d %ux%u\n",
                                count, notify->level, notify->area.x,
                                notify->area.y, notify->area.width,
                                notify->area.height);
                        free(event);
                        return 0;
                    }
                    ++count;
                }
            }
            free(event);
        }
        if (count >= expected)
            return 1;
        if (xcb_connection_has_error(connection) != 0)
            return 0;
        {
            const int64_t now = monotonic_milliseconds();
            struct pollfd descriptor;
            int wait_for;
            int result;
            if (now < 0 || now - started >= timeout_milliseconds) {
                fprintf(stderr, "received %u of %u raw DAMAGE events\n",
                        count, expected);
                return 0;
            }
            wait_for = timeout_milliseconds - (int) (now - started);
            descriptor.fd = xcb_get_file_descriptor(connection);
            descriptor.events = POLLIN;
            descriptor.revents = 0;
            do {
                result = poll(&descriptor, 1, wait_for);
            } while (result < 0 && errno == EINTR);
            if (result <= 0) {
                fprintf(stderr, "received %u of %u raw DAMAGE events\n",
                        count, expected);
                return 0;
            }
        }
    }
    return 1;
}

int
main(void)
{
    xcb_connection_t *connection = xcb_connect(NULL, NULL);
    xcb_connection_t *observer = NULL;
    xcb_screen_t *screen = xcb_setup_roots_iterator(
        xcb_get_setup(connection)).data;
    const xcb_query_extension_reply_t *damage_extension;
    xcb_generic_error_t *error = NULL;
    xcb_damage_query_version_reply_t *damage_version = NULL;
    xcb_xfixes_query_version_reply_t *xfixes_version = NULL;
    xcb_pixmap_t pixmap = XCB_NONE;
    xcb_gcontext_t gc = XCB_NONE;
    xcb_damage_damage_t damage = XCB_NONE;
    xcb_damage_damage_t raw_damage = XCB_NONE;
    xcb_xfixes_region_t repair = XCB_NONE;
    xcb_xfixes_region_t parts = XCB_NONE;
    xcb_rectangle_t rectangle = {2, 3, 8, 6};
    const char *stage = "connecting";
    int passed = 0;
    uint8_t bad_damage;

    if (xcb_connection_has_error(connection) || screen == NULL)
        goto cleanup;
    damage_extension = xcb_get_extension_data(connection, &xcb_damage_id);
    if (damage_extension == NULL || !damage_extension->present)
        goto cleanup;
    bad_damage = damage_extension->first_error;

    stage = "querying extension versions";
    xfixes_version = xcb_xfixes_query_version_reply(
        connection, xcb_xfixes_query_version(connection, 6, 0), &error);
    if (error != NULL || xfixes_version == NULL ||
        xfixes_version->major_version < 2)
        goto cleanup;
    damage_version = xcb_damage_query_version_reply(
        connection, xcb_damage_query_version(connection, 1, 1), &error);
    if (error != NULL || damage_version == NULL ||
        damage_version->major_version != 1 ||
        damage_version->minor_version != 1)
        goto cleanup;

    pixmap = xcb_generate_id(connection);
    gc = xcb_generate_id(connection);
    damage = xcb_generate_id(connection);
    repair = xcb_generate_id(connection);
    parts = xcb_generate_id(connection);
    stage = "creating DAMAGE resources";
    if (!checked(connection,
                 xcb_create_pixmap_checked(
                     connection, screen->root_depth, pixmap, screen->root,
                     30, 20),
                 "CreatePixmap") ||
        !checked(connection,
                 xcb_create_gc_checked(connection, gc, pixmap, 0, NULL),
                 "CreateGC") ||
        !checked(connection,
                 xcb_xfixes_create_region_checked(
                     connection, repair, 1, &rectangle),
                 "XFIXES CreateRegion repair") ||
        !checked(connection,
                 xcb_xfixes_create_region_checked(
                     connection, parts, 0, NULL),
                 "XFIXES CreateRegion parts") ||
        !checked(connection,
                 xcb_damage_create_checked(
                     connection, damage, pixmap,
                     XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY),
                 "DAMAGE Create"))
        goto cleanup;

    stage = "adding explicit damage";
    if (!checked(connection,
                 xcb_damage_add_checked(connection, pixmap, repair),
                 "DAMAGE Add"))
        goto cleanup;
    {
        xcb_damage_notify_event_t *notify = wait_for_damage(
            connection, damage_extension->first_event, pixmap, damage);
        int valid = notify != NULL &&
            (notify->level & 0x7fU) == XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY &&
            notify->area.x == 0 && notify->area.y == 0 &&
            notify->area.width == 30 && notify->area.height == 20 &&
            notify->geometry.x == 0 && notify->geometry.y == 0 &&
            notify->geometry.width == 30 && notify->geometry.height == 20;
        free(notify);
        if (!valid)
            goto cleanup;
    }

    stage = "copying and emptying accumulated damage";
    if (!checked(connection,
                 xcb_damage_subtract_checked(
                     connection, damage, XCB_NONE, parts),
                 "DAMAGE Subtract all") ||
        !region_is(connection, parts, &rectangle))
        goto cleanup;

    rectangle.x = 10;
    rectangle.y = 11;
    rectangle.width = 5;
    rectangle.height = 4;
    stage = "recording core rendering damage";
    if (!checked(connection,
                 xcb_poly_fill_rectangle_checked(
                     connection, pixmap, gc, 1, &rectangle),
                 "PolyFillRectangle"))
        goto cleanup;
    {
        xcb_damage_notify_event_t *notify = wait_for_damage(
            connection, damage_extension->first_event, pixmap, damage);
        int valid = notify != NULL &&
            (notify->level & 0x7fU) == XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY;
        free(notify);
        if (!valid)
            goto cleanup;
    }

    rectangle.width = 3;
    stage = "partially subtracting damage";
    if (!checked(connection,
                 xcb_xfixes_set_region_checked(
                     connection, repair, 1, &rectangle),
                 "XFIXES SetRegion") ||
        !checked(connection,
                 xcb_damage_subtract_checked(
                     connection, damage, repair, parts),
                 "DAMAGE Subtract partial") ||
        !region_is(connection, parts, &rectangle))
        goto cleanup;
    {
        xcb_damage_notify_event_t *notify = wait_for_damage(
            connection, damage_extension->first_event, pixmap, damage);
        free(notify);
        if (notify == NULL)
            goto cleanup;
    }

    rectangle.x = 1;
    rectangle.y = 2;
    rectangle.width = 3;
    rectangle.height = 4;
    stage = "rearming and adding damage";
    if (!checked(connection,
                 xcb_damage_subtract_checked(
                     connection, damage, XCB_NONE, XCB_NONE),
                 "DAMAGE Subtract reset") ||
        !checked(connection,
                 xcb_xfixes_set_region_checked(
                     connection, repair, 1, &rectangle),
                 "XFIXES SetRegion again") ||
        !checked(connection,
                 xcb_damage_add_checked(connection, pixmap, repair),
                 "DAMAGE Add again"))
        goto cleanup;
    {
        xcb_damage_notify_event_t *notify = wait_for_damage(
            connection, damage_extension->first_event, pixmap, damage);
        free(notify);
        if (notify == NULL)
            goto cleanup;
    }

    stage = "delivering successive raw damage to another client";
    observer = xcb_connect(NULL, NULL);
    if (observer == NULL || xcb_connection_has_error(observer) != 0)
        goto cleanup;
    {
        xcb_generic_error_t *observer_error = NULL;
        xcb_damage_query_version_reply_t *observer_version =
            xcb_damage_query_version_reply(
                observer, xcb_damage_query_version(observer, 1, 1),
                &observer_error);
        int valid = observer_error == NULL && observer_version != NULL &&
            observer_version->major_version == 1;
        free(observer_error);
        free(observer_version);
        if (!valid)
            goto cleanup;
    }
    raw_damage = xcb_generate_id(observer);
    if (!checked(observer,
                 xcb_damage_create_checked(
                     observer, raw_damage, pixmap,
                     XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES),
                 "raw DAMAGE Create"))
        goto cleanup;
    rectangle.x = 4;
    rectangle.y = 5;
    rectangle.width = 6;
    rectangle.height = 7;
    {
        unsigned index;
        for (index = 0; index < 31; ++index)
            xcb_poly_fill_rectangle(connection, pixmap, gc, 1, &rectangle);
        if (!checked(connection,
                     xcb_poly_fill_rectangle_checked(
                         connection, pixmap, gc, 1, &rectangle),
                     "final burst PolyFillRectangle"))
            goto cleanup;
    }
    if (!wait_for_damage_count(
            observer, damage_extension->first_event, pixmap, raw_damage,
            32, 2000))
        goto cleanup;
    if (!checked(observer,
                 xcb_damage_destroy_checked(observer, raw_damage),
                 "raw DAMAGE Destroy"))
        goto cleanup;
    raw_damage = XCB_NONE;

    stage = "destroying the DAMAGE object";
    if (!checked(connection,
                 xcb_damage_destroy_checked(connection, damage),
                 "DAMAGE Destroy") ||
        !checked_error(connection,
                       xcb_damage_destroy_checked(connection, damage),
                       bad_damage, "second DAMAGE Destroy"))
        goto cleanup;
    damage = XCB_NONE;
    passed = 1;

cleanup:
    if (!passed)
        fprintf(stderr, "DAMAGE state test failed while %s\n", stage);
    free(error);
    free(damage_version);
    free(xfixes_version);
    if (connection != NULL) {
        if (damage != XCB_NONE)
            xcb_damage_destroy(connection, damage);
        if (repair != XCB_NONE)
            xcb_xfixes_destroy_region(connection, repair);
        if (parts != XCB_NONE)
            xcb_xfixes_destroy_region(connection, parts);
        if (gc != XCB_NONE)
            xcb_free_gc(connection, gc);
        if (pixmap != XCB_NONE)
            xcb_free_pixmap(connection, pixmap);
        xcb_disconnect(connection);
    }
    if (observer != NULL) {
        if (raw_damage != XCB_NONE)
            xcb_damage_destroy(observer, raw_damage);
        xcb_disconnect(observer);
    }
    return passed ? 0 : 1;
}
