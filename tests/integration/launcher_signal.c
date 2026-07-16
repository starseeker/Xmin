#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int
write_all(int fd, const void *data, size_t size)
{
    const char *cursor = data;

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
child_mode(const char *descriptor)
{
    const char *display = getenv("DISPLAY");
    const char *authority = getenv("XAUTHORITY");
    struct stat status;
    char message[128];
    char *end = NULL;
    long descriptor_number = strtol(descriptor, &end, 10);
    int environment_ok;
    int length;

    if (end == descriptor || *end != '\0' || descriptor_number < 0 ||
        descriptor_number > INT_MAX)
        return 2;
    environment_ok = display != NULL && display[0] == ':' &&
        authority != NULL && stat(authority, &status) == 0 &&
        S_ISREG(status.st_mode) && (status.st_mode & 0777) == 0600 &&
        status.st_uid == getuid();
    length = snprintf(message, sizeof(message), "%s\n%lu\n",
                      environment_ok ? display : "ERROR",
                      (unsigned long) getpid());
    if (length < 0 || length >= (int) sizeof(message) ||
        write_all((int) descriptor_number, message, (size_t) length) != 0)
        return 3;
    close((int) descriptor_number);
    if (!environment_ok)
        return 4;
    for (;;)
        pause();
}

static int
read_child_ready(int fd, int *display_out, pid_t *child_out)
{
    char message[128];
    char *first_end;
    char *second_end;
    char *parse_end = NULL;
    size_t used = 0;
    int newline_count = 0;

    while (newline_count < 2 && used + 1 < sizeof(message)) {
        struct pollfd ready = { .fd = fd, .events = POLLIN | POLLHUP };
        ssize_t count;
        int poll_result;
        size_t i;

        do {
            poll_result = poll(&ready, 1, 10000);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result <= 0)
            return -1;
        count = read(fd, message + used, sizeof(message) - used - 1);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        for (i = 0; i < (size_t) count; ++i) {
            if (message[used + i] == '\n')
                ++newline_count;
        }
        used += (size_t) count;
    }
    if (newline_count != 2)
        return -1;
    message[used] = '\0';
    first_end = strchr(message, '\n');
    if (first_end == NULL)
        return -1;
    *first_end = '\0';
    second_end = strchr(first_end + 1, '\n');
    if (second_end == NULL || second_end[1] != '\0' || message[0] != ':')
        return -1;
    *second_end = '\0';
    {
        long display = strtol(message + 1, &parse_end, 10);

        if (parse_end == message + 1 || *parse_end != '\0' || display < 0 ||
            display > 59535)
            return -1;
        *display_out = (int) display;
    }
    {
        long child = strtol(first_end + 1, &parse_end, 10);

        if (parse_end == first_end + 1 || *parse_end != '\0' || child <= 0)
            return -1;
        *child_out = (pid_t) child;
    }
    return 0;
}

static int
find_private_authority(const char *base, char *directory, size_t capacity)
{
    DIR *stream = opendir(base);
    struct dirent *entry;
    int found = 0;

    if (stream == NULL)
        return -1;
    errno = 0;
    while ((entry = readdir(stream)) != NULL) {
        struct stat directory_status;
        struct stat authority_status;
        char authority[PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (++found != 1 || strncmp(entry->d_name, "xmin-run-", 9) != 0 ||
            snprintf(directory, capacity, "%s/%s", base, entry->d_name) >=
                (int) capacity ||
            snprintf(authority, sizeof(authority), "%s/Xauthority", directory) >=
                (int) sizeof(authority) ||
            stat(directory, &directory_status) != 0 ||
            !S_ISDIR(directory_status.st_mode) ||
            (directory_status.st_mode & 0777) != 0700 ||
            directory_status.st_uid != getuid() ||
            stat(authority, &authority_status) != 0 ||
            !S_ISREG(authority_status.st_mode) ||
            (authority_status.st_mode & 0777) != 0600 ||
            authority_status.st_uid != getuid()) {
            closedir(stream);
            return -1;
        }
        errno = 0;
    }
    if (errno != 0) {
        closedir(stream);
        return -1;
    }
    closedir(stream);
    return found == 1 ? 0 : -1;
}

static int
wait_for_process(pid_t process, int *status)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    int i;

    for (i = 0; i < 1000; ++i) {
        pid_t result = waitpid(process, status, WNOHANG);

        if (result == process)
            return 0;
        if (result < 0 && errno != EINTR)
            return -1;
        nanosleep(&delay, NULL);
    }
    return -1;
}

static int
wait_for_pid_to_disappear(pid_t process)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    int i;

    for (i = 0; i < 500; ++i) {
        if (kill(process, 0) != 0) {
            if (errno == ESRCH)
                return 0;
            if (errno != EINTR)
                return -1;
        }
        nanosleep(&delay, NULL);
    }
    return -1;
}

static int
wait_for_path_to_disappear(const char *path)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    struct stat status;
    int i;

    for (i = 0; i < 500; ++i) {
        if (lstat(path, &status) != 0) {
            if (errno == ENOENT)
                return 0;
            if (errno != EINTR)
                return -1;
        }
        nanosleep(&delay, NULL);
    }
    return -1;
}

static int
directory_is_empty(const char *path)
{
    DIR *stream = opendir(path);
    struct dirent *entry;
    int empty = 1;

    if (stream == NULL)
        return -1;
    errno = 0;
    while ((entry = readdir(stream)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            empty = 0;
            break;
        }
        errno = 0;
    }
    if (errno != 0)
        empty = -1;
    closedir(stream);
    return empty;
}

static void
remove_private_state(const char *base)
{
    DIR *stream = opendir(base);
    struct dirent *entry;

    if (stream == NULL)
        return;
    while ((entry = readdir(stream)) != NULL) {
        char directory[PATH_MAX];
        char authority[PATH_MAX];

        if (strncmp(entry->d_name, "xmin-run-", 9) != 0 ||
            snprintf(directory, sizeof(directory), "%s/%s", base,
                     entry->d_name) >= (int) sizeof(directory) ||
            snprintf(authority, sizeof(authority), "%s/Xauthority",
                     directory) >= (int) sizeof(authority))
            continue;
        unlink(authority);
        rmdir(directory);
    }
    closedir(stream);
}

int
main(int argc, char **argv)
{
    char temporary_base[] = "/tmp/xmin-launcher-signal.XXXXXX";
    char private_directory[PATH_MAX] = { 0 };
    char descriptor[24];
    char lock_path[64] = { 0 };
    char socket_path[64] = { 0 };
    int ready_pipe[2] = { -1, -1 };
    int display = -1;
    int launcher_status = 0;
    int result = 1;
    pid_t launcher = -1;
    pid_t command = -1;

    if (argc == 3 && strcmp(argv[1], "--signal-child") == 0)
        return child_mode(argv[2]);
    if (argc != 3) {
        fprintf(stderr, "usage: %s xmin-run Xmin\n", argv[0]);
        return 2;
    }
    if (mkdtemp(temporary_base) == NULL || pipe(ready_pipe) != 0) {
        perror("launcher signal test setup");
        goto cleanup;
    }
    launcher = fork();
    if (launcher < 0) {
        perror("fork");
        goto cleanup;
    }
    if (launcher == 0) {
        close(ready_pipe[0]);
        snprintf(descriptor, sizeof(descriptor), "%d", ready_pipe[1]);
        if (setenv("TMPDIR", temporary_base, 1) != 0)
            _exit(126);
        execl(argv[1], argv[1], "--server", argv[2], "--screen",
              "96x80x24", "--", argv[0], "--signal-child", descriptor,
              (char *) NULL);
        _exit(127);
    }
    close(ready_pipe[1]);
    ready_pipe[1] = -1;
    if (read_child_ready(ready_pipe[0], &display, &command) != 0) {
        fprintf(stderr, "launcher command did not report a secure environment\n");
        goto cleanup;
    }
    close(ready_pipe[0]);
    ready_pipe[0] = -1;
    if (find_private_authority(temporary_base, private_directory,
                               sizeof(private_directory)) != 0) {
        fprintf(stderr, "launcher private authority state was invalid\n");
        goto cleanup;
    }
    if (snprintf(lock_path, sizeof(lock_path), "/tmp/.X%d-lock", display) >=
            (int) sizeof(lock_path) ||
        snprintf(socket_path, sizeof(socket_path), "/tmp/.X11-unix/X%d",
                 display) >= (int) sizeof(socket_path))
        goto cleanup;
    if (kill(launcher, SIGTERM) != 0 ||
        wait_for_process(launcher, &launcher_status) != 0) {
        fprintf(stderr, "launcher did not terminate after SIGTERM\n");
        goto cleanup;
    }
    launcher = -1;
    if (!WIFEXITED(launcher_status) ||
        WEXITSTATUS(launcher_status) != 128 + SIGTERM) {
        fprintf(stderr, "launcher did not preserve the interrupted status\n");
        goto cleanup;
    }
    if (wait_for_pid_to_disappear(command) != 0) {
        fprintf(stderr, "launcher left its command running\n");
        goto cleanup;
    }
    command = -1;
    if (wait_for_path_to_disappear(lock_path) != 0 ||
        wait_for_path_to_disappear(socket_path) != 0) {
        fprintf(stderr, "launcher left its X lock or socket behind\n");
        goto cleanup;
    }
    if (directory_is_empty(temporary_base) != 1) {
        fprintf(stderr, "launcher left private authority state behind\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    if (ready_pipe[0] >= 0)
        close(ready_pipe[0]);
    if (ready_pipe[1] >= 0)
        close(ready_pipe[1]);
    if (launcher > 0) {
        kill(launcher, SIGTERM);
        if (wait_for_process(launcher, &launcher_status) != 0) {
            kill(launcher, SIGKILL);
            waitpid(launcher, &launcher_status, 0);
        }
    }
    if (command > 0 && kill(command, 0) == 0)
        kill(command, SIGKILL);
    if (temporary_base[0] != '\0') {
        remove_private_state(temporary_base);
        rmdir(temporary_base);
    }
    return result;
}
