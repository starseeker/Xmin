#include "xmin_xcb_connect.h"

#include <X11/Xauth.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char auth_protocol[] = "MIT-MAGIC-COOKIE-1";

static void
set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size != 0)
        (void) snprintf(error, error_size, "%s", message);
}

static int
parse_display(const char *display, int *display_number, int *screen_number)
{
    char *end = NULL;
    long display_value;
    long screen_value = 0;

    if (display == NULL || display[0] != ':')
        return -1;
    errno = 0;
    display_value = strtol(display + 1, &end, 10);
    if (errno != 0 || end == display + 1 || display_value < 0 ||
        display_value > 59535)
        return -1;
    if (*end == '.') {
        const char *screen_start = end + 1;

        errno = 0;
        screen_value = strtol(screen_start, &end, 10);
        if (errno != 0 || end == screen_start || screen_value < 0 ||
            screen_value > INT32_MAX)
            return -1;
    }
    if (*end != '\0')
        return -1;
    *display_number = (int) display_value;
    *screen_number = (int) screen_value;
    return 0;
}

static int
open_display_socket(int display_number, char *error, size_t error_size)
{
    struct sockaddr_un address;
    socklen_t address_size;
    size_t path_size;
    int fd;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (snprintf(address.sun_path, sizeof(address.sun_path),
                 "/tmp/.X11-unix/X%d", display_number) >=
        (int) sizeof(address.sun_path)) {
        set_error(error, error_size, "display socket path is too long");
        return -1;
    }
    path_size = strlen(address.sun_path) + 1;
    address_size = (socklen_t) (offsetof(struct sockaddr_un, sun_path) +
                                path_size);
#if defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__OpenBSD__)
    address.sun_len = (uint8_t) address_size;
#endif
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error != NULL && error_size != 0)
            (void) snprintf(error, error_size, "cannot create X11 socket: %s",
                            strerror(errno));
        return -1;
    }
    if (connect(fd, (struct sockaddr *) &address, address_size) != 0) {
        int saved_errno = errno;

        close(fd);
        if (error != NULL && error_size != 0)
            (void) snprintf(error, error_size, "cannot connect to :%d: %s",
                            display_number, strerror(saved_errno));
        return -1;
    }
    return fd;
}

static int
auth_score(const Xauth *auth, const char *display_number,
           size_t display_number_size)
{
    int score;

    if (auth->name_length != sizeof(auth_protocol) - 1 ||
        memcmp(auth->name, auth_protocol, sizeof(auth_protocol) - 1) != 0 ||
        auth->data_length == 0)
        return 0;
    if (auth->number_length != 0 &&
        (auth->number_length != display_number_size ||
         memcmp(auth->number, display_number, display_number_size) != 0))
        return 0;
    if (auth->family == FamilyWild)
        score = 10;
    else if (auth->family == FamilyLocal || auth->family == FamilyLocalHost)
        score = 20;
    else
        return 0;
    if (auth->number_length != 0)
        score += 2;
    return score;
}

static int
read_authority(const char *path, int display_number,
               xcb_auth_info_t *xcb_auth, char *error, size_t error_size)
{
    char number[32];
    size_t number_size;
    FILE *file;
    Xauth *auth;
    int best_score = 0;

    if (snprintf(number, sizeof(number), "%d", display_number) >=
        (int) sizeof(number)) {
        set_error(error, error_size, "invalid display number");
        return -1;
    }
    number_size = strlen(number);
    file = fopen(path, "rb");
    if (file == NULL) {
        if (error != NULL && error_size != 0)
            (void) snprintf(error, error_size, "cannot read Xauthority %s: %s",
                            path, strerror(errno));
        return -1;
    }

    memset(xcb_auth, 0, sizeof(*xcb_auth));
    while ((auth = XauReadAuth(file)) != NULL) {
        int score = auth_score(auth, number, number_size);

        if (score > best_score) {
            char *name = malloc(auth->name_length);
            char *data = malloc(auth->data_length);

            if (name == NULL || data == NULL) {
                free(name);
                free(data);
                XauDisposeAuth(auth);
                fclose(file);
                free(xcb_auth->name);
                free(xcb_auth->data);
                memset(xcb_auth, 0, sizeof(*xcb_auth));
                set_error(error, error_size, "out of memory reading Xauthority");
                return -1;
            }
            memcpy(name, auth->name, auth->name_length);
            memcpy(data, auth->data, auth->data_length);
            free(xcb_auth->name);
            free(xcb_auth->data);
            xcb_auth->name = name;
            xcb_auth->namelen = auth->name_length;
            xcb_auth->data = data;
            xcb_auth->datalen = auth->data_length;
            best_score = score;
        }
        XauDisposeAuth(auth);
    }
    fclose(file);
    if (best_score == 0) {
        if (error != NULL && error_size != 0)
            (void) snprintf(error, error_size,
                            "Xauthority %s has no cookie for :%d", path,
                            display_number);
        return -1;
    }
    return 0;
}

static const char *
connection_error_message(int code)
{
    switch (code) {
    case XCB_CONN_ERROR:
        return "X11 connection failed";
    case XCB_CONN_CLOSED_EXT_NOTSUPPORTED:
        return "required X11 extension is unavailable";
    case XCB_CONN_CLOSED_MEM_INSUFFICIENT:
        return "out of memory opening X11 connection";
    case XCB_CONN_CLOSED_REQ_LEN_EXCEED:
        return "X11 request length exceeded";
    case XCB_CONN_CLOSED_PARSE_ERR:
        return "invalid X11 display string";
    case XCB_CONN_CLOSED_INVALID_SCREEN:
        return "requested X11 screen does not exist";
    case XCB_CONN_CLOSED_FDPASSING_FAILED:
        return "X11 file-descriptor passing failed";
    default:
        return "unknown X11 connection error";
    }
}

int
xmin_xcb_connect(xmin_xcb_session *session, const char *display,
                 char *error, size_t error_size)
{
    const xcb_setup_t *setup;
    xcb_screen_iterator_t screens;
    const char *authority_path;
    xcb_auth_info_t auth;
    xcb_auth_info_t *auth_pointer = NULL;
    int display_number;
    int screen_number;
    int fd;
    int index;

    memset(session, 0, sizeof(*session));
    if (display == NULL)
        display = getenv("DISPLAY");
    if (parse_display(display, &display_number, &screen_number) != 0) {
        set_error(error, error_size,
                  "DISPLAY must name a local Xmin display such as :20 or :20.0");
        return -1;
    }

    memset(&auth, 0, sizeof(auth));
    authority_path = getenv("XAUTHORITY");
    if (authority_path != NULL && authority_path[0] != '\0') {
        if (read_authority(authority_path, display_number, &auth,
                           error, error_size) != 0)
            return -1;
        auth_pointer = &auth;
    }
    fd = open_display_socket(display_number, error, error_size);
    if (fd < 0) {
        free(auth.name);
        free(auth.data);
        return -1;
    }
    session->connection = xcb_connect_to_fd(fd, auth_pointer);
    free(auth.name);
    free(auth.data);
    if (session->connection == NULL) {
        close(fd);
        set_error(error, error_size, "libxcb could not allocate a connection");
        return -1;
    }
    if (xcb_connection_has_error(session->connection) != 0) {
        set_error(error, error_size,
                  connection_error_message(
                      xcb_connection_has_error(session->connection)));
        xmin_xcb_disconnect(session);
        return -1;
    }

    setup = xcb_get_setup(session->connection);
    screens = xcb_setup_roots_iterator(setup);
    for (index = 0; index < screen_number && screens.rem != 0; ++index)
        xcb_screen_next(&screens);
    if (screens.rem == 0) {
        set_error(error, error_size, "requested X11 screen does not exist");
        xmin_xcb_disconnect(session);
        return -1;
    }
    session->screen = screens.data;
    session->display_number = display_number;
    session->screen_number = screen_number;
    return 0;
}

void
xmin_xcb_disconnect(xmin_xcb_session *session)
{
    if (session->connection != NULL)
        xcb_disconnect(session->connection);
    memset(session, 0, sizeof(*session));
}
