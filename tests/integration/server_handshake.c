#include "xmin/config.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    X_PROTOCOL_MAJOR = 11,
    X_QUERY_EXTENSION = 98,
    X_REPLY = 1,
    X_ERROR = 0,
    X_BAD_LENGTH = 16,
    X_GET_GEOMETRY = 14,
    X_GET_INPUT_FOCUS = 43,
    SETUP_PREFIX_SIZE = 8,
    SETUP_FIXED_SIZE = 32,
    REPLY_SIZE = 32
};

static int little_endian;

static void
put16(unsigned char *out, uint16_t value)
{
    if (little_endian) {
        out[0] = (unsigned char) value;
        out[1] = (unsigned char) (value >> 8);
    }
    else {
        out[0] = (unsigned char) (value >> 8);
        out[1] = (unsigned char) value;
    }
}

static uint16_t
get16(const unsigned char *in)
{
    if (little_endian)
        return (uint16_t) (in[0] | ((uint16_t) in[1] << 8));
    return (uint16_t) (((uint16_t) in[0] << 8) | in[1]);
}

static void
put32(unsigned char *out, uint32_t value)
{
    if (little_endian) {
        out[0] = (unsigned char) value;
        out[1] = (unsigned char) (value >> 8);
        out[2] = (unsigned char) (value >> 16);
        out[3] = (unsigned char) (value >> 24);
    }
    else {
        out[0] = (unsigned char) (value >> 24);
        out[1] = (unsigned char) (value >> 16);
        out[2] = (unsigned char) (value >> 8);
        out[3] = (unsigned char) value;
    }
}

static uint32_t
get32(const unsigned char *in)
{
    if (little_endian) {
        return (uint32_t) in[0] | ((uint32_t) in[1] << 8) |
            ((uint32_t) in[2] << 16) | ((uint32_t) in[3] << 24);
    }
    return ((uint32_t) in[0] << 24) | ((uint32_t) in[1] << 16) |
        ((uint32_t) in[2] << 8) | (uint32_t) in[3];
}

static int
write_all(int fd, const void *data, size_t size)
{
    const unsigned char *cursor = data;

    while (size != 0) {
        ssize_t count = write(fd, cursor, size);

        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        cursor += (size_t) count;
        size -= (size_t) count;
    }
    return 0;
}

static int
read_all(int fd, void *data, size_t size)
{
    unsigned char *cursor = data;

    while (size != 0) {
        ssize_t count = read(fd, cursor, size);

        if (count == 0)
            return -1;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        cursor += (size_t) count;
        size -= (size_t) count;
    }
    return 0;
}

static int
read_display_number(int fd, char *display, size_t capacity)
{
    struct pollfd ready = { .fd = fd, .events = POLLIN | POLLHUP };
    size_t used = 0;

    if (poll(&ready, 1, 15000) <= 0)
        return -1;
    while (used + 1 < capacity) {
        char byte;
        ssize_t count = read(fd, &byte, 1);

        if (count < 0 && errno == EINTR)
            continue;
        if (count != 1)
            return -1;
        if (byte == '\n') {
            display[used] = '\0';
            return used == 0 ? -1 : 0;
        }
        if (byte < '0' || byte > '9')
            return -1;
        display[used++] = byte;
    }
    return -1;
}

static int
query_extension(int fd, const char *name, uint16_t sequence)
{
    unsigned char request[128] = { 0 };
    unsigned char reply[REPLY_SIZE];
    size_t name_size = strlen(name);
    size_t request_size = 8 + ((name_size + 3) & ~(size_t) 3);

    if (request_size > sizeof(request))
        return -1;
    request[0] = X_QUERY_EXTENSION;
    put16(request + 2, (uint16_t) (request_size / 4));
    put16(request + 4, (uint16_t) name_size);
    memcpy(request + 8, name, name_size);

    if (write_all(fd, request, request_size) != 0 ||
        read_all(fd, reply, sizeof(reply)) != 0)
        return -1;
    if (reply[0] != X_REPLY || get16(reply + 2) != sequence || reply[8] == 0) {
        fprintf(stderr, "required X11 extension is unavailable: %s\n", name);
        return -1;
    }
    return 0;
}

static int
stop_server(pid_t child)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    int status;
    int i;

    if (kill(child, SIGTERM) != 0 && errno != ESRCH)
        return -1;
    for (i = 0; i < 500; ++i) {
        pid_t result = waitpid(child, &status, WNOHANG);

        if (result == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        if (result < 0 && errno != EINTR)
            return errno == ECHILD ? 0 : -1;
        nanosleep(&delay, NULL);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    return -1;
}

int
main(int argc, char **argv)
{
    static const char *const required_extensions[] = {
        "Generic Event Extension",
        "SHAPE",
        "XInputExtension",
        "XTEST",
        "BIG-REQUESTS",
        "SYNC",
        "XKEYBOARD",
        "XC-MISC",
        "XFIXES",
        "RENDER",
        "RANDR",
        "Composite",
        "DAMAGE",
        "MIT-SCREEN-SAVER",
        "DOUBLE-BUFFER",
        "Present",
        "XINERAMA",
#if XMIN_BUILD_INDIRECT_GLX
        "GLX"
#endif
    };
    unsigned char client_prefix[12] = { 0 };
    unsigned char server_prefix[SETUP_PREFIX_SIZE];
    unsigned char *setup = NULL;
    unsigned char *swapped_setup = NULL;
    char display[8];
    char display_fd[16];
    char socket_path[sizeof(((struct sockaddr_un *) 0)->sun_path)] = { 0 };
    struct sockaddr_un address;
    uint16_t endian_probe = 1;
    uint16_t vendor_size;
    uint16_t sequence = 1;
    size_t setup_size;
    size_t i;
    int ready_pipe[2] = { -1, -1 };
    int client = -1;
    int swapped_client = -1;
    int result = 1;
    pid_t child = -1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/Xmin\n", argv[0]);
        return 2;
    }
    little_endian = *(unsigned char *) &endian_probe == 1;

    if (pipe(ready_pipe) != 0) {
        perror("pipe");
        return 3;
    }
    child = fork();
    if (child < 0) {
        perror("fork");
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return 4;
    }
    if (child == 0) {
        close(ready_pipe[0]);
        snprintf(display_fd, sizeof(display_fd), "%d", ready_pipe[1]);
        execl(argv[1], argv[1], "-displayfd", display_fd, "-ac", "-terminate",
              "-screen", "0", "320x240x24", (char *) NULL);
        _exit(127);
    }

    close(ready_pipe[1]);
    ready_pipe[1] = -1;
    if (read_display_number(ready_pipe[0], display, sizeof(display)) != 0) {
        fprintf(stderr, "Xmin did not report a ready display\n");
        goto cleanup;
    }
    close(ready_pipe[0]);
    ready_pipe[0] = -1;

    if (snprintf(socket_path, sizeof(socket_path), "/tmp/.X11-unix/X%s",
                 display) >= (int) sizeof(socket_path))
        goto cleanup;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0 || connect(client, (struct sockaddr *) &address,
                              sizeof(address)) != 0) {
        perror("connect");
        goto cleanup;
    }

    client_prefix[0] = little_endian ? 'l' : 'B';
    put16(client_prefix + 2, X_PROTOCOL_MAJOR);
    if (write_all(client, client_prefix, sizeof(client_prefix)) != 0 ||
        read_all(client, server_prefix, sizeof(server_prefix)) != 0) {
        fprintf(stderr, "X11 connection setup did not complete\n");
        goto cleanup;
    }
    if (server_prefix[0] != X_REPLY ||
        get16(server_prefix + 2) != X_PROTOCOL_MAJOR) {
        fprintf(stderr, "Xmin rejected the X11 protocol handshake\n");
        goto cleanup;
    }
    setup_size = (size_t) get16(server_prefix + 6) * 4;
    if (setup_size < SETUP_FIXED_SIZE || setup_size > 1024 * 1024)
        goto cleanup;
    setup = malloc(setup_size);
    if (setup == NULL || read_all(client, setup, setup_size) != 0)
        goto cleanup;
    vendor_size = get16(setup + 16);
    if (setup[20] != 1 || SETUP_FIXED_SIZE + vendor_size > setup_size ||
        vendor_size != strlen(XMIN_NAME) ||
        memcmp(setup + SETUP_FIXED_SIZE, XMIN_NAME, vendor_size) != 0) {
        fprintf(stderr, "unexpected X11 connection setup metadata\n");
        goto cleanup;
    }

    for (i = 0; i < sizeof(required_extensions) / sizeof(required_extensions[0]);
         ++i, ++sequence) {
        if (query_extension(client, required_extensions[i], sequence) != 0)
            goto cleanup;
    }
#if XMIN_HAVE_MITSHM
    if (query_extension(client, "MIT-SHM", sequence) != 0)
        goto cleanup;
#endif

    swapped_client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (swapped_client < 0 ||
        connect(swapped_client, (struct sockaddr *) &address,
                sizeof(address)) != 0) {
        fprintf(stderr, "opposite-endian client could not connect\n");
        goto cleanup;
    }
    little_endian = !little_endian;
    memset(client_prefix, 0, sizeof(client_prefix));
    client_prefix[0] = little_endian ? 'l' : 'B';
    put16(client_prefix + 2, X_PROTOCOL_MAJOR);
    if (write_all(swapped_client, client_prefix, sizeof(client_prefix)) != 0 ||
        read_all(swapped_client, server_prefix, sizeof(server_prefix)) != 0 ||
        server_prefix[0] != X_REPLY ||
        get16(server_prefix + 2) != X_PROTOCOL_MAJOR) {
        fprintf(stderr, "opposite-endian X11 handshake failed\n");
        goto cleanup;
    }
    setup_size = (size_t) get16(server_prefix + 6) * 4;
    if (setup_size < SETUP_FIXED_SIZE || setup_size > 1024 * 1024)
        goto cleanup;
    swapped_setup = malloc(setup_size);
    if (swapped_setup == NULL ||
        read_all(swapped_client, swapped_setup, setup_size) != 0)
        goto cleanup;
    vendor_size = get16(swapped_setup + 16);
    if (swapped_setup[20] != 1 ||
        SETUP_FIXED_SIZE + vendor_size > setup_size ||
        vendor_size != strlen(XMIN_NAME) ||
        memcmp(swapped_setup + SETUP_FIXED_SIZE, XMIN_NAME, vendor_size) != 0) {
        fprintf(stderr, "opposite-endian setup metadata was invalid\n");
        goto cleanup;
    }
    if (query_extension(swapped_client, "RENDER", 1) != 0)
        goto cleanup;
    {
        unsigned char request[8] = { 0 };
        unsigned char geometry[REPLY_SIZE];
        size_t root_offset = SETUP_FIXED_SIZE +
            ((vendor_size + 3U) & ~3U) + (size_t) swapped_setup[21] * 8U;
        uint32_t root;

        if (root_offset + 40 > setup_size)
            goto cleanup;
        root = get32(swapped_setup + root_offset);
        request[0] = X_GET_GEOMETRY;
        put16(request + 2, 2);
        put32(request + 4, root);
        if (write_all(swapped_client, request, sizeof(request)) != 0 ||
            read_all(swapped_client, geometry, sizeof(geometry)) != 0 ||
            geometry[0] != X_REPLY || get16(geometry + 2) != 2 ||
            get16(geometry + 16) != 320 || get16(geometry + 18) != 240) {
            fprintf(stderr, "opposite-endian GetGeometry failed\n");
            goto cleanup;
        }
    }
    {
        unsigned char malformed[4] = { 0 };
        unsigned char error_reply[REPLY_SIZE];
        unsigned char focus_request[4] = { 0 };
        unsigned char focus_reply[REPLY_SIZE];

        malformed[0] = X_GET_GEOMETRY;
        put16(malformed + 2, 1);
        if (write_all(swapped_client, malformed, sizeof(malformed)) != 0 ||
            read_all(swapped_client, error_reply, sizeof(error_reply)) != 0 ||
            error_reply[0] != X_ERROR || error_reply[1] != X_BAD_LENGTH ||
            get16(error_reply + 2) != 3 ||
            error_reply[10] != X_GET_GEOMETRY) {
            fprintf(stderr, "opposite-endian core BadLength handling failed\n");
            goto cleanup;
        }
        focus_request[0] = X_GET_INPUT_FOCUS;
        put16(focus_request + 2, 1);
        if (write_all(swapped_client, focus_request,
                      sizeof(focus_request)) != 0 ||
            read_all(swapped_client, focus_reply, sizeof(focus_reply)) != 0 ||
            focus_reply[0] != X_REPLY || get16(focus_reply + 2) != 4) {
            fprintf(stderr, "client did not survive malformed core request\n");
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    free(swapped_setup);
    free(setup);
    if (swapped_client >= 0)
        close(swapped_client);
    if (client >= 0)
        close(client);
    if (ready_pipe[0] >= 0)
        close(ready_pipe[0]);
    if (ready_pipe[1] >= 0)
        close(ready_pipe[1]);
    if (child > 0 && stop_server(child) != 0) {
        fprintf(stderr, "Xmin did not shut down cleanly\n");
        result = 1;
    }
    return result;
}
