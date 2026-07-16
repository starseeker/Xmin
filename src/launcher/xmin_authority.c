#include "xmin_launcher.h"

#include "xmin/config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if XMIN_HAVE_GETRANDOM
#include <sys/random.h>
#endif

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
        if (count == 0) {
            errno = EIO;
            return -1;
        }
        cursor += (size_t) count;
        size -= (size_t) count;
    }
    return 0;
}

static int
write_u16(int fd, uint16_t value)
{
    const unsigned char bytes[2] = {
        (unsigned char) (value >> 8),
        (unsigned char) value
    };

    return write_all(fd, bytes, sizeof(bytes));
}

static int
write_field(int fd, const void *data, size_t size)
{
    if (size > UINT16_MAX || write_u16(fd, (uint16_t) size) != 0)
        return -1;
    return size == 0 ? 0 : write_all(fd, data, size);
}

int
xmin_random_bytes(unsigned char *buffer, size_t size)
{
    if (buffer == NULL && size != 0) {
        errno = EINVAL;
        return -1;
    }

#if XMIN_HAVE_GETRANDOM
    while (size != 0) {
        ssize_t count = getrandom(buffer, size, 0);

        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0) {
            errno = EIO;
            return -1;
        }
        buffer += (size_t) count;
        size -= (size_t) count;
    }
    return 0;
#elif XMIN_HAVE_GETENTROPY
    while (size != 0) {
        size_t chunk = size > 256 ? 256 : size;

        if (getentropy(buffer, chunk) != 0)
            return -1;
        buffer += chunk;
        size -= chunk;
    }
    return 0;
#elif XMIN_HAVE_ARC4RANDOM_BUF
    arc4random_buf(buffer, size);
    return 0;
#else
#error "xmin-run requires getrandom, getentropy, or arc4random_buf"
#endif
}

int
xmin_write_authority(const char *path,
                     const unsigned char *cookie,
                     size_t cookie_size)
{
    static const char protocol[] = "MIT-MAGIC-COOKIE-1";
    int flags = O_WRONLY | O_CREAT | O_EXCL;
    int fd;
    int result = -1;

    if (path == NULL || cookie == NULL || cookie_size == 0 ||
        cookie_size > UINT16_MAX) {
        errno = EINVAL;
        return -1;
    }
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    fd = open(path, flags, 0600);
    if (fd < 0)
        return -1;

    /* FamilyWild with empty address and display fields is deliberately used:
     * the server display is selected only after this file has been passed to
     * -auth, and libXau treats empty fields as wildcards for the client. */
    if (write_u16(fd, UINT16_MAX) == 0 &&
        write_field(fd, NULL, 0) == 0 &&
        write_field(fd, NULL, 0) == 0 &&
        write_field(fd, protocol, sizeof(protocol) - 1) == 0 &&
        write_field(fd, cookie, cookie_size) == 0 &&
        fsync(fd) == 0) {
        if (close(fd) == 0)
            result = 0;
    }
    else {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
    }

    if (result != 0)
        unlink(path);
    return result;
}
