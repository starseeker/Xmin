#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

typedef struct {
    uint8_t major_opcode;
    uint8_t minor_opcode;
    uint16_t length;
    xcb_window_t window;
    xcb_atom_t name;
    uint8_t primary;
    uint8_t automatic;
    uint16_t output_count;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t millimetre_width;
    uint32_t millimetre_height;
} xmin_randr_set_monitor_request_t;

_Static_assert(sizeof(xmin_randr_set_monitor_request_t) == 32,
               "RANDR SetMonitor wire request must be 32 bytes");

static xcb_void_cookie_t
set_monitor_checked(xcb_connection_t *connection, xcb_window_t window,
                    xcb_atom_t name, int16_t x, int16_t y,
                    uint16_t width, uint16_t height,
                    uint32_t millimetre_width,
                    uint32_t millimetre_height)
{
    static const xcb_protocol_request_t protocol = {
        .count = 2,
        .ext = &xcb_randr_id,
        .opcode = XCB_RANDR_SET_MONITOR,
        .isvoid = 1
    };
    xmin_randr_set_monitor_request_t request = {
        .window = window,
        .name = name,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .millimetre_width = millimetre_width,
        .millimetre_height = millimetre_height
    };
    struct iovec parts[4];
    xcb_void_cookie_t cookie;

    parts[2].iov_base = &request;
    parts[2].iov_len = sizeof(request);
    parts[3].iov_base = NULL;
    parts[3].iov_len = 0;
    cookie.sequence = xcb_send_request(
        connection, XCB_REQUEST_CHECKED, parts + 2, &protocol);
    return cookie;
}

static int
checked(xcb_connection_t *connection, xcb_void_cookie_t cookie,
        const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (error == NULL)
        return 1;
    fprintf(stderr, "%s failed with X11 error %u\n", operation,
            error->error_code);
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
                operation, error == NULL ? 0U : error->error_code, expected);
    }
    free(error);
    return result;
}

static int
reply_error(xcb_generic_error_t **error, uint8_t expected,
            const char *operation)
{
    int result = *error != NULL && (*error)->error_code == expected;
    if (!result) {
        fprintf(stderr, "%s returned X11 error %u instead of %u\n",
                operation, *error == NULL ? 0U : (*error)->error_code,
                expected);
    }
    free(*error);
    *error = NULL;
    return result;
}

static xcb_atom_t
atom(xcb_connection_t *connection, const char *name)
{
    xcb_generic_error_t *error = NULL;
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        connection, xcb_intern_atom(connection, 0, strlen(name), name),
        &error);
    xcb_atom_t result = reply == NULL ? XCB_NONE : reply->atom;
    free(error);
    free(reply);
    return result;
}

int
main(void)
{
    xcb_connection_t *connection = xcb_connect(NULL, NULL);
    const xcb_setup_t *setup = xcb_get_setup(connection);
    xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
    xcb_generic_error_t *error = NULL;
    xcb_randr_query_version_reply_t *version = NULL;
    xcb_randr_get_screen_resources_current_reply_t *resources = NULL;
    xcb_randr_get_screen_resources_reply_t *legacy_resources = NULL;
    xcb_randr_get_output_info_reply_t *output = NULL;
    xcb_randr_get_crtc_info_reply_t *crtc = NULL;
    xcb_randr_get_crtc_gamma_reply_t *gamma = NULL;
    xcb_randr_get_crtc_transform_reply_t *transform = NULL;
    xcb_randr_get_panning_reply_t *panning = NULL;
    xcb_randr_set_panning_reply_t *set_panning = NULL;
    xcb_randr_get_providers_reply_t *providers = NULL;
    xcb_randr_get_monitors_reply_t *monitors = NULL;
    xcb_randr_get_output_property_reply_t *property = NULL;
    xcb_randr_query_output_property_reply_t *property_info = NULL;
    xcb_randr_list_output_properties_reply_t *property_list = NULL;
    xcb_randr_get_crtc_gamma_size_reply_t *gamma_size = NULL;
    xcb_randr_set_screen_config_reply_t *screen_config = NULL;
    xcb_randr_get_screen_info_reply_t *screen_info = NULL;
    xcb_randr_get_screen_size_range_reply_t *size_range = NULL;
    xcb_randr_get_output_primary_reply_t *primary = NULL;
    xcb_randr_mode_info_t mode_info;
    xcb_randr_create_mode_reply_t *created = NULL;
    xcb_randr_set_crtc_config_reply_t *configured = NULL;
    xcb_randr_output_t output_id = XCB_NONE;
    xcb_randr_crtc_t crtc_id = XCB_NONE;
    xcb_randr_mode_t mode_id = XCB_NONE;
    xcb_atom_t property_atom = XCB_NONE;
    xcb_atom_t monitor_atom = XCB_NONE;
    xcb_render_transform_t identity = {
        65536, 0, 0,
        0, 65536, 0,
        0, 0, 65536
    };
    uint16_t red[256], green[256], blue[256];
    uint32_t property_value = 0x584d494eU;
    uint32_t pending_property_value = 0x50454e44U;
    const int32_t property_range[2] = {0, 100};
    const char *stage = "connecting";
    int result = 0;
    int i;
    uint16_t initial_width;
    uint16_t initial_height;
    uint8_t randr_bad_crtc;
    uint8_t randr_bad_provider;

    if (xcb_connection_has_error(connection) || screen == NULL)
        goto cleanup;
    initial_width = screen->width_in_pixels;
    initial_height = screen->height_in_pixels;
    randr_bad_crtc = (uint8_t) (
        xcb_get_extension_data(connection, &xcb_randr_id)->first_error + 1);
    randr_bad_provider = (uint8_t) (
        xcb_get_extension_data(connection, &xcb_randr_id)->first_error + 3);
    stage = "querying version";
    version = xcb_randr_query_version_reply(
        connection, xcb_randr_query_version(connection, 1, 6), &error);
    if (error != NULL || version == NULL || version->major_version != 1 ||
        version->minor_version != 6)
        goto cleanup;

    stage = "selecting notifications";
    if (!checked(connection,
                 xcb_randr_select_input_checked(
                     connection, screen->root,
                     XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
                         XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
                         XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
                         XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY |
                         XCB_RANDR_NOTIFY_MASK_RESOURCE_CHANGE),
                 "RANDR SelectInput"))
        goto cleanup;

    stage = "querying resources";
    resources = xcb_randr_get_screen_resources_current_reply(
        connection,
        xcb_randr_get_screen_resources_current(connection, screen->root),
        &error);
    if (error != NULL || resources == NULL || resources->num_crtcs != 1 ||
        resources->num_outputs != 1 || resources->num_modes < 1)
        goto cleanup;
    output_id = xcb_randr_get_screen_resources_current_outputs(resources)[0];
    crtc_id = xcb_randr_get_screen_resources_current_crtcs(resources)[0];

    stage = "querying legacy screen views";
    legacy_resources = xcb_randr_get_screen_resources_reply(
        connection,
        xcb_randr_get_screen_resources(connection, screen->root), &error);
    screen_info = xcb_randr_get_screen_info_reply(
        connection, xcb_randr_get_screen_info(connection, screen->root),
        &error);
    size_range = xcb_randr_get_screen_size_range_reply(
        connection,
        xcb_randr_get_screen_size_range(connection, screen->root), &error);
    screen_config = xcb_randr_set_screen_config_reply(
        connection,
        xcb_randr_set_screen_config(
            connection, screen->root, XCB_CURRENT_TIME,
            resources->config_timestamp, 0, XCB_RANDR_ROTATION_ROTATE_0, 0),
        &error);
    if (error != NULL || legacy_resources == NULL ||
        legacy_resources->num_crtcs != 1 ||
        legacy_resources->num_outputs != 1 || screen_info == NULL ||
        size_range == NULL ||
        screen_config == NULL || screen_info->nSizes < 1 ||
        size_range->min_width == 0 ||
        size_range->max_width < initial_width ||
        screen_config->status != XCB_RANDR_SET_CONFIG_SUCCESS)
        goto cleanup;

    stage = "querying output and CRTC";
    output = xcb_randr_get_output_info_reply(
        connection,
        xcb_randr_get_output_info(connection, output_id,
                                  resources->config_timestamp),
        &error);
    crtc = xcb_randr_get_crtc_info_reply(
        connection,
        xcb_randr_get_crtc_info(connection, crtc_id,
                                resources->config_timestamp),
        &error);
    if (error != NULL || output == NULL || crtc == NULL ||
        output->connection != XCB_RANDR_CONNECTION_CONNECTED ||
        output->crtc != crtc_id || output->num_modes < 1 ||
        crtc->width != initial_width || crtc->height != initial_height ||
        crtc->num_outputs != 1) {
        fprintf(stderr,
                "output=%p crtc=%p connection=%u output_crtc=0x%x "
                "expected_crtc=0x%x modes=%u geometry=%ux%u outputs=%u\n",
                (void *) output, (void *) crtc,
                output == NULL ? 255U : output->connection,
                output == NULL ? 0U : output->crtc, crtc_id,
                output == NULL ? 0U : output->num_modes,
                crtc == NULL ? 0U : crtc->width,
                crtc == NULL ? 0U : crtc->height,
                crtc == NULL ? 0U : crtc->num_outputs);
        goto cleanup;
    }

    stage = "round-tripping gamma";
    gamma_size = xcb_randr_get_crtc_gamma_size_reply(
        connection, xcb_randr_get_crtc_gamma_size(connection, crtc_id),
        &error);
    if (error != NULL || gamma_size == NULL || gamma_size->size != 256)
        goto cleanup;
    for (i = 0; i < 256; ++i) {
        red[i] = (uint16_t) (65535U - i * 257U);
        green[i] = (uint16_t) (i * 257U);
        blue[i] = (uint16_t) (i * 193U);
    }
    if (!checked(connection,
                 xcb_randr_set_crtc_gamma_checked(
                     connection, crtc_id, 256, red, green, blue),
                 "RANDR SetCrtcGamma"))
        goto cleanup;
    gamma = xcb_randr_get_crtc_gamma_reply(
        connection, xcb_randr_get_crtc_gamma(connection, crtc_id), &error);
    if (error != NULL || gamma == NULL || gamma->size != 256 ||
        memcmp(xcb_randr_get_crtc_gamma_red(gamma), red, sizeof(red)) != 0 ||
        memcmp(xcb_randr_get_crtc_gamma_green(gamma), green,
               sizeof(green)) != 0 ||
        memcmp(xcb_randr_get_crtc_gamma_blue(gamma), blue,
               sizeof(blue)) != 0)
        goto cleanup;

    stage = "querying transform and panning";
    {
        xcb_generic_error_t *transform_error = xcb_request_check(
            connection,
            xcb_randr_set_crtc_transform_checked(
                connection, crtc_id, identity, 7, "nearest", 0, NULL));
        if (transform_error != NULL &&
            transform_error->error_code != XCB_VALUE) {
            free(transform_error);
            goto cleanup;
        }
        free(transform_error);
    }
    {
        xcb_generic_error_t *panning_error = NULL;
        set_panning = xcb_randr_set_panning_reply(
            connection,
            xcb_randr_set_panning(
                connection, crtc_id, XCB_CURRENT_TIME,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            &panning_error);
        if (panning_error != NULL &&
            panning_error->error_code != XCB_MATCH &&
            panning_error->error_code != XCB_VALUE &&
            panning_error->error_code != randr_bad_crtc) {
            fprintf(stderr, "SetPanning returned X11 error %u\n",
                    panning_error->error_code);
            free(panning_error);
            goto cleanup;
        }
        free(panning_error);
    }
    transform = xcb_randr_get_crtc_transform_reply(
        connection, xcb_randr_get_crtc_transform(connection, crtc_id),
        &error);
    panning = xcb_randr_get_panning_reply(
        connection, xcb_randr_get_panning(connection, crtc_id), &error);
    if (error != NULL || transform == NULL || panning == NULL ||
        (set_panning != NULL &&
         set_panning->status != XCB_RANDR_SET_CONFIG_SUCCESS) ||
        transform->current_transform.matrix11 != 65536 ||
        transform->current_transform.matrix22 != 65536 ||
        transform->current_transform.matrix33 != 65536) {
        fprintf(stderr,
                "set_panning=%p transform=%p panning=%p error=%u status=%u "
                "diagonal=%d,%d,%d\n",
                (void *) set_panning, (void *) transform, (void *) panning,
                error == NULL ? 0U : error->error_code,
                set_panning == NULL ? 255U : set_panning->status,
                transform == NULL ? 0 : transform->current_transform.matrix11,
                transform == NULL ? 0 : transform->current_transform.matrix22,
                transform == NULL ? 0 : transform->current_transform.matrix33);
        goto cleanup;
    }

    stage = "checking provider and monitor profile";
    providers = xcb_randr_get_providers_reply(
        connection, xcb_randr_get_providers(connection, screen->root),
        &error);
    monitors = xcb_randr_get_monitors_reply(
        connection, xcb_randr_get_monitors(connection, screen->root, 1),
        &error);
    if (error != NULL || providers == NULL || monitors == NULL ||
        providers->num_providers != 0 || monitors->nMonitors != 1 ||
        monitors->nOutputs != 1)
        goto cleanup;

    stage = "rejecting provider operations";
    {
        const xcb_randr_provider_t invalid_provider = 0xdeadbeefU;
        void *failed_reply;

        failed_reply = xcb_randr_get_provider_info_reply(
            connection,
            xcb_randr_get_provider_info(
                connection, invalid_provider, XCB_CURRENT_TIME),
            &error);
        const int unexpected_provider_info = failed_reply != NULL;
        free(failed_reply);
        if (unexpected_provider_info ||
            !reply_error(&error, randr_bad_provider,
                         "RANDR GetProviderInfo"))
            goto cleanup;
        if (!checked_error(
                connection,
                xcb_randr_set_provider_offload_sink_checked(
                    connection, invalid_provider, XCB_NONE,
                    XCB_CURRENT_TIME),
                randr_bad_provider, "RANDR SetProviderOffloadSink") ||
            !checked_error(
                connection,
                xcb_randr_set_provider_output_source_checked(
                    connection, invalid_provider, XCB_NONE,
                    XCB_CURRENT_TIME),
                randr_bad_provider, "RANDR SetProviderOutputSource"))
            goto cleanup;
        failed_reply = xcb_randr_list_provider_properties_reply(
            connection,
            xcb_randr_list_provider_properties(
                connection, invalid_provider),
            &error);
        const int unexpected_provider_properties = failed_reply != NULL;
        free(failed_reply);
        if (unexpected_provider_properties ||
            !reply_error(&error, randr_bad_provider,
                         "RANDR ListProviderProperties"))
            goto cleanup;
        failed_reply = xcb_randr_query_provider_property_reply(
            connection,
            xcb_randr_query_provider_property(
                connection, invalid_provider, XCB_ATOM_INTEGER),
            &error);
        const int unexpected_provider_property = failed_reply != NULL;
        free(failed_reply);
        if (unexpected_provider_property ||
            !reply_error(&error, randr_bad_provider,
                         "RANDR QueryProviderProperty") ||
            !checked_error(
                connection,
                xcb_randr_configure_provider_property_checked(
                    connection, invalid_provider, XCB_ATOM_INTEGER,
                    0, 0, 0, NULL),
                randr_bad_provider, "RANDR ConfigureProviderProperty") ||
            !checked_error(
                connection,
                xcb_randr_change_provider_property_checked(
                    connection, invalid_provider, XCB_ATOM_INTEGER,
                    XCB_ATOM_INTEGER, 32, XCB_PROP_MODE_REPLACE,
                    1, &property_value),
                randr_bad_provider, "RANDR ChangeProviderProperty") ||
            !checked_error(
                connection,
                xcb_randr_delete_provider_property_checked(
                    connection, invalid_provider, XCB_ATOM_INTEGER),
                randr_bad_provider, "RANDR DeleteProviderProperty"))
            goto cleanup;
        failed_reply = xcb_randr_get_provider_property_reply(
            connection,
            xcb_randr_get_provider_property(
                connection, invalid_provider, XCB_ATOM_INTEGER,
                XCB_ATOM_INTEGER, 0, 1, 0, 0),
            &error);
        const int unexpected_get_provider_property = failed_reply != NULL;
        free(failed_reply);
        if (unexpected_get_provider_property ||
            !reply_error(&error, randr_bad_provider,
                         "RANDR GetProviderProperty"))
            goto cleanup;
    }

    stage = "rejecting DRM leases";
    {
        xcb_randr_lease_t lease = xcb_generate_id(connection);
        void *failed_reply = xcb_randr_create_lease_reply(
            connection,
            xcb_randr_create_lease(
                connection, screen->root, lease, 1, 1,
                &crtc_id, &output_id),
            &error);
        const int unexpected_lease = failed_reply != NULL;
        free(failed_reply);
        if (unexpected_lease ||
            !reply_error(&error, XCB_MATCH, "RANDR CreateLease") ||
            !checked_error(
                connection, xcb_randr_free_lease_checked(
                    connection, lease, 1),
                XCB_VALUE, "RANDR FreeLease"))
            goto cleanup;
    }

    stage = "round-tripping a monitor";
    monitor_atom = atom(connection, "XMIN-TEST-MONITOR");
    if (monitor_atom == XCB_NONE) {
        goto cleanup;
    }
    /*
     * libxcb 1.15's generated SetMonitor wrapper leaves its trailing iovec
     * uninitialised.  Encode this one small request directly so the server
     * conformance test remains deterministic across host libxcb versions.
     */
    if (!checked(connection,
                 set_monitor_checked(connection, screen->root, monitor_atom,
                                     5, 6, 20, 15, 5, 4),
                 "RANDR SetMonitor"))
        goto cleanup;
    free(monitors);
    monitors = xcb_randr_get_monitors_reply(
        connection, xcb_randr_get_monitors(connection, screen->root, 0),
        &error);
    if (error != NULL || monitors == NULL || monitors->nMonitors < 2) {
        fprintf(stderr,
                "GetMonitors returned error=%u monitors=%p count=%u "
                "outputs=%u\n",
                error == NULL ? 0U : error->error_code, (void *) monitors,
                monitors == NULL ? 0U : monitors->nMonitors,
                monitors == NULL ? 0U : monitors->nOutputs);
        fprintf(stderr, "XCB connection error=%d\n",
                xcb_connection_has_error(connection));
        goto cleanup;
    }
    if (!checked(connection,
                 xcb_randr_delete_monitor_checked(
                     connection, screen->root, monitor_atom),
                 "RANDR DeleteMonitor"))
        goto cleanup;

    stage = "round-tripping an output property";
    property_atom = atom(connection, "_XMIN_SERVER_RANDR_TEST");
    if (property_atom == XCB_NONE ||
        !checked(connection,
                 xcb_randr_configure_output_property_checked(
                     connection, output_id, property_atom, 0, 1, 2,
                     property_range),
                 "RANDR ConfigureOutputProperty"))
        goto cleanup;
    property_info = xcb_randr_query_output_property_reply(
        connection,
        xcb_randr_query_output_property(
            connection, output_id, property_atom),
        &error);
    property_list = xcb_randr_list_output_properties_reply(
        connection, xcb_randr_list_output_properties(connection, output_id),
        &error);
    if (error != NULL || property_info == NULL || property_list == NULL ||
        !property_info->range ||
        xcb_randr_query_output_property_valid_values_length(property_info) !=
            2 ||
        property_list->num_atoms < 1 ||
        !checked(connection,
                 xcb_randr_change_output_property_checked(
                     connection, output_id, property_atom, XCB_ATOM_INTEGER,
                     32, XCB_PROP_MODE_REPLACE, 1, &property_value),
                 "RANDR ChangeOutputProperty"))
        goto cleanup;
    property = xcb_randr_get_output_property_reply(
        connection,
        xcb_randr_get_output_property(
            connection, output_id, property_atom, XCB_ATOM_INTEGER,
            0, 1, 0, 0),
        &error);
    if (error != NULL || property == NULL || property->format != 32 ||
        property->num_items != 1 ||
        memcmp(xcb_randr_get_output_property_data(property), &property_value,
               sizeof(property_value)) != 0)
        goto cleanup;

    stage = "separating pending output property state";
    if (!checked(connection,
                 xcb_randr_configure_output_property_checked(
                     connection, output_id, property_atom, 1, 0, 0, NULL),
                 "RANDR pending ConfigureOutputProperty") ||
        !checked(connection,
                 xcb_randr_change_output_property_checked(
                     connection, output_id, property_atom, XCB_ATOM_INTEGER,
                     32, XCB_PROP_MODE_REPLACE, 1,
                     &pending_property_value),
                 "RANDR pending ChangeOutputProperty"))
        goto cleanup;
    free(property);
    property = xcb_randr_get_output_property_reply(
        connection,
        xcb_randr_get_output_property(
            connection, output_id, property_atom, XCB_ATOM_INTEGER,
            0, 1, 0, 1),
        &error);
    if (error != NULL || property == NULL || property->num_items != 1 ||
        memcmp(xcb_randr_get_output_property_data(property),
               &pending_property_value, sizeof(pending_property_value)) != 0)
        goto cleanup;
    free(property);
    property = xcb_randr_get_output_property_reply(
        connection,
        xcb_randr_get_output_property(
            connection, output_id, property_atom, XCB_ATOM_INTEGER,
            0, 1, 0, 0),
        &error);
    if (error != NULL || property == NULL || property->num_items != 1 ||
        memcmp(xcb_randr_get_output_property_data(property), &property_value,
               sizeof(property_value)) != 0)
        goto cleanup;

    stage = "setting primary output";
    if (!checked(connection,
                 xcb_randr_set_output_primary_checked(
                     connection, screen->root, output_id),
                 "RANDR SetOutputPrimary"))
        goto cleanup;
    primary = xcb_randr_get_output_primary_reply(
        connection,
        xcb_randr_get_output_primary(connection, screen->root), &error);
    if (error != NULL || primary == NULL || primary->output != output_id)
        goto cleanup;

    stage = "creating and selecting a mode";
    memset(&mode_info, 0, sizeof(mode_info));
    mode_info.width = 80;
    mode_info.height = 60;
    mode_info.dot_clock = 288000;
    mode_info.hsync_start = 80;
    mode_info.hsync_end = 80;
    mode_info.htotal = 80;
    mode_info.vsync_start = 60;
    mode_info.vsync_end = 60;
    mode_info.vtotal = 60;
    mode_info.name_len = 5;
    created = xcb_randr_create_mode_reply(
        connection,
        xcb_randr_create_mode(connection, screen->root, mode_info, 5,
                              "80x60"),
        &error);
    if (error != NULL || created == NULL || created->mode == XCB_NONE)
        goto cleanup;
    mode_id = created->mode;
    if (!checked(connection,
                 xcb_randr_add_output_mode_checked(
                     connection, output_id, mode_id),
                 "RANDR AddOutputMode"))
        goto cleanup;
    configured = xcb_randr_set_crtc_config_reply(
        connection,
        xcb_randr_set_crtc_config(
            connection, crtc_id, XCB_CURRENT_TIME,
            XCB_CURRENT_TIME, 0, 0, mode_id,
            XCB_RANDR_ROTATION_ROTATE_0, 1, &output_id),
        &error);
    if (error != NULL || configured == NULL ||
        configured->status != XCB_RANDR_SET_CONFIG_SUCCESS)
        goto cleanup;

    stage = "resizing the framebuffer";
    if (!checked(connection,
                 xcb_randr_set_screen_size_checked(
                     connection, screen->root, 96, 80, 25, 21),
                 "RANDR SetScreenSize"))
        goto cleanup;
    free(crtc);
    crtc = xcb_randr_get_crtc_info_reply(
        connection,
        xcb_randr_get_crtc_info(connection, crtc_id, XCB_CURRENT_TIME),
        &error);
    if (error != NULL || crtc == NULL || crtc->mode != mode_id ||
        crtc->width != 80 || crtc->height != 60)
        goto cleanup;
    {
        xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(
            connection, xcb_get_geometry(connection, screen->root), &error);
        if (error != NULL || geometry == NULL || geometry->width != 96 ||
            geometry->height != 80) {
            free(geometry);
            goto cleanup;
        }
        free(geometry);
    }

    stage = "removing mutable RANDR resources";
    if (!checked(connection,
                 xcb_randr_delete_output_property_checked(
                     connection, output_id, property_atom),
                 "RANDR DeleteOutputProperty"))
        goto cleanup;
    free(configured);
    configured = xcb_randr_set_crtc_config_reply(
        connection,
        xcb_randr_set_crtc_config(
            connection, crtc_id, XCB_CURRENT_TIME, XCB_CURRENT_TIME,
            0, 0, XCB_NONE, XCB_RANDR_ROTATION_ROTATE_0, 0, NULL),
        &error);
    if (error != NULL || configured == NULL ||
        configured->status != XCB_RANDR_SET_CONFIG_SUCCESS ||
        !checked(connection,
                 xcb_randr_delete_output_mode_checked(
                     connection, output_id, mode_id),
                 "RANDR DeleteOutputMode") ||
        !checked(connection,
                 xcb_randr_destroy_mode_checked(connection, mode_id),
                 "RANDR DestroyMode"))
        goto cleanup;

    result = 1;

cleanup:
    if (!result) {
        fprintf(stderr, "RANDR state test failed while %s", stage);
        if (error != NULL)
            fprintf(stderr, " (X11 error %u)", error->error_code);
        fputc('\n', stderr);
    }
    free(configured);
    free(created);
    free(property);
    free(primary);
    free(gamma_size);
    free(property_list);
    free(property_info);
    free(size_range);
    free(screen_info);
    free(screen_config);
    free(monitors);
    free(providers);
    free(panning);
    free(set_panning);
    free(transform);
    free(gamma);
    free(crtc);
    free(output);
    free(resources);
    free(legacy_resources);
    free(version);
    free(error);
    xcb_disconnect(connection);
    return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
