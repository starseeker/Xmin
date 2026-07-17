#include "xmin/config.h"

#include <xcb/shm.h>
#include <xcb/xcb.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <unistd.h>

enum { WIDTH = 8, HEIGHT = 6, BYTE_COUNT = 4096 };

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
expect_error(xcb_connection_t *connection, xcb_void_cookie_t cookie,
             uint8_t expected, const char *operation)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    const int matched = error != NULL && error->error_code == expected;
    if (!matched) {
        fprintf(stderr, "%s returned %s%u instead of %u\n", operation,
                error == NULL ? "no error/" : "error ",
                error == NULL ? 0 : error->error_code, expected);
    }
    free(error);
    return matched;
}

static int
read_pixel(xcb_connection_t *connection, xcb_drawable_t drawable,
           uint32_t expected)
{
    xcb_generic_error_t *error = NULL;
    xcb_get_image_reply_t *reply = xcb_get_image_reply(
        connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, drawable,
                      WIDTH / 2, HEIGHT / 2, 1, 1, UINT32_MAX),
        &error);
    uint32_t pixel = 0;
    if (reply != NULL && xcb_get_image_data_length(reply) >= 4)
        memcpy(&pixel, xcb_get_image_data(reply), sizeof(pixel));
    const int matched = error == NULL && reply != NULL &&
        (pixel & 0x00ffffffU) == expected;
    if (!matched)
        fprintf(stderr, "MIT-SHM readback was 0x%08x, expected 0x%08x\n",
                pixel, expected);
    free(error);
    free(reply);
    return matched;
}

static int
put_and_check(xcb_connection_t *connection, xcb_drawable_t drawable,
              xcb_gcontext_t graphics, xcb_shm_seg_t segment,
              uint32_t expected, uint8_t completion)
{
    if (!checked(connection,
                 xcb_shm_put_image_checked(
                     connection, drawable, graphics, WIDTH, HEIGHT, 0, 0,
                     WIDTH, HEIGHT, 0, 0, 24, XCB_IMAGE_FORMAT_Z_PIXMAP,
                     completion, segment, 0),
                 "MIT-SHM PutImage")) {
        return 0;
    }
    if (completion) {
        xcb_generic_event_t *event = xcb_wait_for_event(connection);
        const xcb_shm_completion_event_t *complete =
            (const xcb_shm_completion_event_t *) event;
        const xcb_query_extension_reply_t *extension =
            xcb_get_extension_data(connection, &xcb_shm_id);
        const int valid = event != NULL && extension != NULL &&
            (event->response_type & 0x7fU) == extension->first_event &&
            complete->drawable == drawable && complete->shmseg == segment &&
            complete->major_event == extension->major_opcode &&
            complete->minor_event == XCB_SHM_PUT_IMAGE;
        free(event);
        if (!valid) {
            fprintf(stderr, "invalid MIT-SHM Completion event\n");
            return 0;
        }
    }
    return read_pixel(connection, drawable, expected);
}

int
main(void)
{
#if !XMIN_HAVE_MITSHM
    return 77;
#else
    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    if (xcb_connection_has_error(connection)) {
        fprintf(stderr, "unable to connect to X server\n");
        return 1;
    }
    xcb_screen_iterator_t screens =
        xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (screen_number-- > 0)
        xcb_screen_next(&screens);
    xcb_screen_t *screen = screens.data;
    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(connection, &xcb_shm_id);
    xcb_generic_error_t *error = NULL;
    xcb_shm_query_version_reply_t *version = xcb_shm_query_version_reply(
        connection, xcb_shm_query_version(connection), &error);
    if (extension == NULL || !extension->present || error != NULL ||
        version == NULL || version->major_version != 1 ||
        version->minor_version < 2 || version->shared_pixmaps != 0) {
        fprintf(stderr, "unexpected MIT-SHM discovery/version reply\n");
        free(error);
        free(version);
        xcb_disconnect(connection);
        return 1;
    }
    free(version);

    xcb_window_t window = xcb_generate_id(connection);
    xcb_gcontext_t graphics = xcb_generate_id(connection);
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      0, 0, WIDTH, HEIGHT, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, NULL);
    xcb_create_gc(connection, graphics, window, 0, NULL);
    xcb_map_window(connection, window);

    int result = 1;
    int shmid = shmget(IPC_PRIVATE, BYTE_COUNT, IPC_CREAT | 0600);
    uint32_t *pixels = shmid < 0 ? (void *) -1 : shmat(shmid, NULL, 0);
    xcb_shm_seg_t segment = xcb_generate_id(connection);
    if (shmid < 0 || pixels == (void *) -1) {
        fprintf(stderr, "unable to allocate SysV shared memory\n");
        result = 0;
        goto cleanup;
    }
    for (size_t index = 0; index < WIDTH * HEIGHT; ++index)
        pixels[index] = 0x0000ff00U;
    if (!checked(connection,
                 xcb_shm_attach_checked(connection, segment, (uint32_t) shmid,
                                        0),
                 "MIT-SHM Attach")) {
        result = 0;
        goto cleanup;
    }
    shmctl(shmid, IPC_RMID, NULL);
    shmid = -1;
    if (!put_and_check(connection, window, graphics, segment, 0x0000ff00U, 1))
        result = 0;

    memset(pixels, 0, BYTE_COUNT);
    xcb_shm_get_image_reply_t *get = xcb_shm_get_image_reply(
        connection,
        xcb_shm_get_image(connection, window, 0, 0, WIDTH, HEIGHT,
                          UINT32_MAX, XCB_IMAGE_FORMAT_Z_PIXMAP, segment, 128),
        &error);
    uint32_t downloaded = 0;
    memcpy(&downloaded, (const uint8_t *) pixels + 128, sizeof(downloaded));
    if (error != NULL || get == NULL || get->size != WIDTH * HEIGHT * 4 ||
        (downloaded & 0x00ffffffU) != 0x0000ff00U) {
        fprintf(stderr, "MIT-SHM GetImage failed\n");
        result = 0;
    }
    free(error);
    error = NULL;
    free(get);
    if (!expect_error(
            connection,
            xcb_shm_put_image_checked(
                connection, window, graphics, 64, 64, 0, 0, 64, 64, 0, 0,
                24, XCB_IMAGE_FORMAT_Z_PIXMAP, 0, segment, BYTE_COUNT - 4),
            XCB_ACCESS, "out-of-bounds MIT-SHM PutImage")) {
        result = 0;
    }
    xcb_pixmap_t shared_pixmap = xcb_generate_id(connection);
    if (!expect_error(
            connection,
            xcb_shm_create_pixmap_checked(
                connection, shared_pixmap, window, WIDTH, HEIGHT, 24,
                segment, 0),
            XCB_MATCH, "unsupported MIT-SHM CreatePixmap")) {
        result = 0;
    }
    if (!checked(connection, xcb_shm_detach_checked(connection, segment),
                 "MIT-SHM Detach")) {
        result = 0;
    }
    if (!expect_error(connection,
                      xcb_shm_detach_checked(connection, segment),
                      extension->first_error + XCB_SHM_BAD_SEG,
                      "duplicate MIT-SHM Detach")) {
        result = 0;
    }
    shmdt(pixels);
    pixels = (void *) -1;

    int readonly_id = shmget(IPC_PRIVATE, BYTE_COUNT, IPC_CREAT | 0600);
    uint32_t *readonly_pixels = readonly_id < 0
        ? (void *) -1
        : shmat(readonly_id, NULL, 0);
    xcb_shm_seg_t readonly_segment = xcb_generate_id(connection);
    if (readonly_id < 0 || readonly_pixels == (void *) -1 ||
        !checked(connection,
                 xcb_shm_attach_checked(
                     connection, readonly_segment, (uint32_t) readonly_id, 1),
                 "read-only MIT-SHM Attach")) {
        fprintf(stderr, "unable to attach read-only shared memory\n");
        result = 0;
    }
    else {
        shmctl(readonly_id, IPC_RMID, NULL);
        readonly_id = -1;
        xcb_shm_get_image_reply_t *readonly_reply =
            xcb_shm_get_image_reply(
                connection,
                xcb_shm_get_image(
                    connection, window, 0, 0, 1, 1, UINT32_MAX,
                    XCB_IMAGE_FORMAT_Z_PIXMAP, readonly_segment, 0),
                &error);
        if (error == NULL || error->error_code != XCB_ACCESS ||
            readonly_reply != NULL) {
            fprintf(stderr, "MIT-SHM wrote through a read-only attachment\n");
            result = 0;
        }
        free(error);
        error = NULL;
        free(readonly_reply);
        if (!checked(connection,
                     xcb_shm_detach_checked(connection, readonly_segment),
                     "read-only MIT-SHM Detach")) {
            result = 0;
        }
    }
    if (readonly_pixels != (void *) -1)
        shmdt(readonly_pixels);
    if (readonly_id >= 0)
        shmctl(readonly_id, IPC_RMID, NULL);

    char path[] = "/tmp/xmin-shm-test-XXXXXX";
    int backing = mkstemp(path);
    unlink(path);
    if (backing < 0 || ftruncate(backing, BYTE_COUNT) != 0) {
        fprintf(stderr, "unable to allocate descriptor shared memory\n");
        result = 0;
        if (backing >= 0)
            close(backing);
        goto cleanup;
    }
    uint32_t *mapped = mmap(NULL, BYTE_COUNT, PROT_READ | PROT_WRITE,
                            MAP_SHARED, backing, 0);
    if (mapped == MAP_FAILED) {
        close(backing);
        result = 0;
        goto cleanup;
    }
    for (size_t index = 0; index < WIDTH * HEIGHT; ++index)
        mapped[index] = 0x000000ffU;
    xcb_shm_seg_t fd_segment = xcb_generate_id(connection);
    int sent = dup(backing);
    if (sent < 0 ||
        !checked(connection,
                 xcb_shm_attach_fd_checked(connection, fd_segment, sent, 0),
                 "MIT-SHM AttachFd") ||
        !put_and_check(connection, window, graphics, fd_segment,
                       0x000000ffU, 0) ||
        !checked(connection,
                 xcb_shm_detach_checked(connection, fd_segment),
                 "MIT-SHM descriptor Detach")) {
        result = 0;
    }
    munmap(mapped, BYTE_COUNT);
    close(backing);

    xcb_shm_seg_t created_segment = xcb_generate_id(connection);
    xcb_shm_create_segment_reply_t *created = xcb_shm_create_segment_reply(
        connection,
        xcb_shm_create_segment(connection, created_segment, BYTE_COUNT, 0),
        &error);
    if (error != NULL || created == NULL || created->nfd != 1) {
        fprintf(stderr, "MIT-SHM CreateSegment failed\n");
        result = 0;
    }
    else {
        int *descriptors =
            xcb_shm_create_segment_reply_fds(connection, created);
        uint32_t *created_pixels = mmap(
            NULL, BYTE_COUNT, PROT_READ | PROT_WRITE, MAP_SHARED,
            descriptors[0], 0);
        if (created_pixels == MAP_FAILED) {
            result = 0;
        }
        else {
            for (size_t index = 0; index < WIDTH * HEIGHT; ++index)
                created_pixels[index] = 0x00ffff00U;
            if (!put_and_check(connection, window, graphics, created_segment,
                               0x00ffff00U, 0)) {
                result = 0;
            }
            munmap(created_pixels, BYTE_COUNT);
        }
        close(descriptors[0]);
        if (!checked(connection,
                     xcb_shm_detach_checked(connection, created_segment),
                     "MIT-SHM created-segment Detach")) {
            result = 0;
        }
    }
    free(error);
    free(created);

cleanup:
    if (pixels != (void *) -1)
        shmdt(pixels);
    if (shmid >= 0)
        shmctl(shmid, IPC_RMID, NULL);
    xcb_free_gc(connection, graphics);
    xcb_destroy_window(connection, window);
    xcb_disconnect(connection);
    return result ? 0 : 1;
#endif
}
