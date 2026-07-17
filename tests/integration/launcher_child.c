#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

enum {
    X_PROTOCOL_MAJOR = 11,
    SETUP_PREFIX_SIZE = 8
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
        if (count == 0)
            return -1;
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
connect_display(const char *display)
{
    struct sockaddr_un address;
    char path[sizeof(address.sun_path)];
    char *end = NULL;
    long number;
    int fd;

    if (display == NULL || display[0] != ':')
        return -1;
    number = strtol(display + 1, &end, 10);
    if (end == display + 1 || *end != '\0' || number < 0 || number > 59535)
        return -1;
    if (snprintf(path, sizeof(path), "/tmp/.X11-unix/X%ld", number) >=
        (int) sizeof(path))
        return -1;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int
handshake(const char *display,
          const void *protocol,
          size_t protocol_size,
          const void *cookie,
          size_t cookie_size,
          int expect_success)
{
    unsigned char request[128] = { 0 };
    unsigned char prefix[SETUP_PREFIX_SIZE];
    unsigned char *additional = NULL;
    size_t protocol_padded = (protocol_size + 3) & ~(size_t) 3;
    size_t cookie_padded = (cookie_size + 3) & ~(size_t) 3;
    size_t request_size = 12 + protocol_padded + cookie_padded;
    size_t additional_size;
    int fd;
    int result = -1;

    if (request_size > sizeof(request) || protocol_size > UINT16_MAX ||
        cookie_size > UINT16_MAX)
        return -1;
    fd = connect_display(display);
    if (fd < 0)
        return -1;

    request[0] = little_endian ? 'l' : 'B';
    put16(request + 2, X_PROTOCOL_MAJOR);
    put16(request + 6, (uint16_t) protocol_size);
    put16(request + 8, (uint16_t) cookie_size);
    if (protocol_size != 0)
        memcpy(request + 12, protocol, protocol_size);
    if (cookie_size != 0)
        memcpy(request + 12 + protocol_padded, cookie, cookie_size);

    if (write_all(fd, request, request_size) != 0 ||
        read_all(fd, prefix, sizeof(prefix)) != 0)
        goto cleanup;
    additional_size = (size_t) get16(prefix + 6) * 4;
    if (additional_size > 1024 * 1024)
        goto cleanup;
    if (additional_size != 0) {
        additional = malloc(additional_size);
        if (additional == NULL || read_all(fd, additional, additional_size) != 0)
            goto cleanup;
    }
    if ((prefix[0] != 0) == expect_success &&
        (!expect_success || get16(prefix + 2) == X_PROTOCOL_MAJOR))
        result = 0;

cleanup:
    free(additional);
    close(fd);
    return result;
}

int
main(void)
{
    static const char expected_protocol[] = "MIT-MAGIC-COOKIE-1";
    const char *display = getenv("DISPLAY");
    const char *authority_path = getenv("XAUTHORITY");
    struct stat status;
    uint16_t endian_probe = 1;
    unsigned char authority[128];
    size_t authority_size;
    const unsigned char *authority_name;
    const unsigned char *authority_data;
    uint16_t authority_name_size;
    uint16_t authority_data_size;
    int result = 1;

    little_endian = *(unsigned char *) &endian_probe == 1;
    if (display == NULL || authority_path == NULL ||
        stat(authority_path, &status) != 0 || !S_ISREG(status.st_mode) ||
        (status.st_mode & 077) != 0 || status.st_uid != getuid()) {
        fprintf(stderr, "xmin-run supplied an insecure or incomplete environment\n");
        return 2;
    }
    FILE *authority_file = fopen(authority_path, "rb");
    if (authority_file == NULL)
        return 3;
    authority_size = fread(authority, 1, sizeof(authority), authority_file);
    if (ferror(authority_file) || !feof(authority_file)) {
        fclose(authority_file);
        return 4;
    }
    fclose(authority_file);
    if (authority_size < 12 || authority[0] != 0xff ||
        authority[1] != 0xff || authority[2] != 0 || authority[3] != 0 ||
        authority[4] != 0 || authority[5] != 0)
        return 4;
    authority_name_size = (uint16_t) (((uint16_t) authority[6] << 8) |
                                      authority[7]);
    authority_name = authority + 8;
    if ((size_t) authority_name_size + 10 > authority_size)
        return 4;
    authority_data_size = (uint16_t)
        (((uint16_t) authority[8 + authority_name_size] << 8) |
         authority[9 + authority_name_size]);
    authority_data = authority + 10 + authority_name_size;
    if (authority_name_size != sizeof(expected_protocol) - 1 ||
        memcmp(authority_name, expected_protocol,
               sizeof(expected_protocol) - 1) != 0 ||
        authority_data_size != 16 ||
        10 + (size_t) authority_name_size + authority_data_size !=
            authority_size) {
        return 4;
    }

    if (handshake(display, NULL, 0, NULL, 0, 0) != 0) {
        fprintf(stderr, "Xmin accepted an unauthenticated launcher client\n");
        goto cleanup;
    }
    if (handshake(display, authority_name, authority_name_size,
                  authority_data, authority_data_size, 1) != 0) {
        fprintf(stderr, "Xmin rejected the launcher cookie\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    return result;
}
