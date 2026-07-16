#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    FIRST_TEST_DISPLAY = 50000,
    LAST_TEST_DISPLAY = 50999,
    X_PROTOCOL_MAJOR = 11,
    SETUP_PREFIX_SIZE = 8
};

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
path_is_absent(const char *path)
{
    struct stat status;

    if (lstat(path, &status) == 0)
        return 0;
    return errno == ENOENT ? 1 : -1;
}

static int
create_stale_lock(char *lock_path,
                  size_t lock_capacity,
                  char *socket_path,
                  size_t socket_capacity,
                  int *display_out,
                  char stale_contents[12])
{
    static const int stale_pid = 99999999;
    int display;

    snprintf(stale_contents, 12, "%10d\n", stale_pid);
    for (display = FIRST_TEST_DISPLAY; display <= LAST_TEST_DISPLAY;
         ++display) {
        int fd;
        int lock_absent;
        int socket_absent;

        if (snprintf(lock_path, lock_capacity, "/tmp/.X%d-lock", display) >=
                (int) lock_capacity ||
            snprintf(socket_path, socket_capacity, "/tmp/.X11-unix/X%d",
                     display) >= (int) socket_capacity)
            return -1;
        lock_absent = path_is_absent(lock_path);
        socket_absent = path_is_absent(socket_path);
        if (lock_absent < 0 || socket_absent < 0)
            return -1;
        if (!lock_absent || !socket_absent)
            continue;

        fd = open(lock_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            return -1;
        }
        if (write_all(fd, stale_contents, 11) != 0 ||
            fchmod(fd, 0444) != 0 || close(fd) != 0) {
            int saved_errno = errno;

            close(fd);
            unlink(lock_path);
            errno = saved_errno;
            return -1;
        }
        *display_out = display;
        return 0;
    }
    errno = EADDRINUSE;
    return -1;
}

static void
remove_lock_if_unchanged(const char *path, const char expected[12])
{
    char contents[12];
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return;
    if (read_all(fd, contents, 11) == 0 && memcmp(contents, expected, 11) == 0)
        unlink(path);
    close(fd);
}

static int
read_display_number(int fd, int expected)
{
    struct pollfd ready = { .fd = fd, .events = POLLIN | POLLHUP };
    char display[16];
    char *end = NULL;
    size_t used = 0;
    long number;
    int poll_result;

    do {
        poll_result = poll(&ready, 1, 15000);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result <= 0)
        return -1;
    while (used + 1 < sizeof(display)) {
        char byte;
        ssize_t count = read(fd, &byte, 1);

        if (count < 0 && errno == EINTR)
            continue;
        if (count != 1)
            return -1;
        if (byte == '\n') {
            display[used] = '\0';
            number = strtol(display, &end, 10);
            return end != display && *end == '\0' && number == expected ?
                0 : -1;
        }
        if (byte < '0' || byte > '9')
            return -1;
        display[used++] = byte;
    }
    return -1;
}

static int
connect_and_handshake(const char *socket_path)
{
    unsigned char request[12] = { 0 };
    unsigned char prefix[SETUP_PREFIX_SIZE];
    unsigned char *setup = NULL;
    struct sockaddr_un address;
    uint16_t endian_probe = 1;
    uint16_t setup_words;
    size_t setup_size;
    int client = -1;
    int result = -1;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(address.sun_path))
        return -1;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0 ||
        connect(client, (struct sockaddr *) &address, sizeof(address)) != 0)
        goto cleanup;

    request[0] = *(unsigned char *) &endian_probe == 1 ? 'l' : 'B';
    if (request[0] == 'l') {
        request[2] = X_PROTOCOL_MAJOR;
        request[3] = 0;
    }
    else {
        request[2] = 0;
        request[3] = X_PROTOCOL_MAJOR;
    }
    if (write_all(client, request, sizeof(request)) != 0 ||
        read_all(client, prefix, sizeof(prefix)) != 0 || prefix[0] != 1)
        goto cleanup;
    if (request[0] == 'l')
        setup_words = (uint16_t) (prefix[6] | ((uint16_t) prefix[7] << 8));
    else
        setup_words = (uint16_t) (((uint16_t) prefix[6] << 8) | prefix[7]);
    setup_size = (size_t) setup_words * 4;
    if (setup_size == 0 || setup_size > 1024 * 1024)
        goto cleanup;
    setup = malloc(setup_size);
    if (setup == NULL || read_all(client, setup, setup_size) != 0)
        goto cleanup;
    result = 0;

cleanup:
    free(setup);
    if (client >= 0)
        close(client);
    return result;
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

static int
wait_for_paths_to_disappear(const char *lock_path, const char *socket_path)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    int i;

    for (i = 0; i < 500; ++i) {
        int lock_absent = path_is_absent(lock_path);
        int socket_absent = path_is_absent(socket_path);

        if (lock_absent == 1 && socket_absent == 1)
            return 0;
        if (lock_absent < 0 || socket_absent < 0)
            return -1;
        nanosleep(&delay, NULL);
    }
    return -1;
}

int
main(int argc, char **argv)
{
    char stale_contents[12];
    char lock_path[64];
    char socket_path[sizeof(((struct sockaddr_un *) 0)->sun_path)];
    char display_argument[24];
    char display_fd[24];
    char server_contents[12];
    int display = -1;
    int ready_pipe[2] = { -1, -1 };
    int result = 1;
    pid_t child = -1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/Xmin\n", argv[0]);
        return 2;
    }
    if (create_stale_lock(lock_path, sizeof(lock_path), socket_path,
                          sizeof(socket_path), &display, stale_contents) != 0) {
        perror("cannot create isolated stale X lock");
        return 3;
    }
    if (pipe(ready_pipe) != 0) {
        perror("pipe");
        goto cleanup;
    }
    snprintf(display_argument, sizeof(display_argument), ":%d", display);
    snprintf(display_fd, sizeof(display_fd), "%d", ready_pipe[1]);
    child = fork();
    if (child < 0) {
        perror("fork");
        goto cleanup;
    }
    if (child == 0) {
        close(ready_pipe[0]);
        execl(argv[1], argv[1], display_argument, "-displayfd", display_fd,
              "-ac", "-noreset", "-screen", "0", "96x80x24",
              (char *) NULL);
        _exit(127);
    }
    close(ready_pipe[1]);
    ready_pipe[1] = -1;
    if (read_display_number(ready_pipe[0], display) != 0) {
        fprintf(stderr, "Xmin did not recover the stale display lock\n");
        goto cleanup;
    }
    close(ready_pipe[0]);
    ready_pipe[0] = -1;
    if (connect_and_handshake(socket_path) != 0) {
        fprintf(stderr, "recovered display did not accept an X11 handshake\n");
        goto cleanup;
    }
    if (stop_server(child) != 0) {
        fprintf(stderr, "Xmin did not shut down cleanly after lock recovery\n");
        child = -1;
        goto cleanup;
    }
    child = -1;
    if (wait_for_paths_to_disappear(lock_path, socket_path) != 0) {
        fprintf(stderr, "Xmin left a lock or socket after shutdown\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    if (ready_pipe[0] >= 0)
        close(ready_pipe[0]);
    if (ready_pipe[1] >= 0)
        close(ready_pipe[1]);
    if (child > 0) {
        snprintf(server_contents, sizeof(server_contents), "%10lu\n",
                 (unsigned long) child);
        stop_server(child);
        remove_lock_if_unchanged(lock_path, server_contents);
    }
    remove_lock_if_unchanged(lock_path, stale_contents);
    return result;
}
