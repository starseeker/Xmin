#include "xmin_launcher.h"

#include "xmin/config.h"

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

static volatile sig_atomic_t pending_signal;

static void
record_signal(int signal_number)
{
    pending_signal = signal_number;
}

static int
install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = record_signal;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) == 0 &&
        sigaction(SIGTERM, &action, NULL) == 0 &&
        sigaction(SIGHUP, &action, NULL) == 0 ? 0 : -1;
}

static void
reset_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
}

static void
print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "usage: %s [--server PATH] [--screen WxHxD] [--dpi DPI] -- "
            "COMMAND [ARG ...]\n",
            program);
}

static char *
default_server_path(const char *launcher_path)
{
    const char *slash = strrchr(launcher_path, '/');
    char *path;
    size_t directory_size;

    if (slash == NULL)
        return strdup("Xmin");
    directory_size = (size_t) (slash - launcher_path) + 1;
    path = malloc(directory_size + sizeof("Xmin"));
    if (path == NULL)
        return NULL;
    memcpy(path, launcher_path, directory_size);
    memcpy(path + directory_size, "Xmin", sizeof("Xmin"));
    return path;
}

static char *
resolve_launcher_path(const char *program)
{
    const char *path_environment;
    const char *cursor;

    if (strchr(program, '/') != NULL) {
        char *resolved = realpath(program, NULL);

        return resolved != NULL ? resolved : strdup(program);
    }
    path_environment = getenv("PATH");
    if (path_environment == NULL)
        return strdup(program);
    cursor = path_environment;
    while (1) {
        const char *end = strchr(cursor, ':');
        size_t directory_size = end != NULL ? (size_t) (end - cursor) :
            strlen(cursor);
        const char *directory = cursor;
        char *candidate;
        char *resolved;
        size_t size;

        if (directory_size == 0) {
            directory = ".";
            directory_size = 1;
        }
        size = directory_size + 1 + strlen(program) + 1;
        candidate = malloc(size);
        if (candidate == NULL)
            return NULL;
        snprintf(candidate, size, "%.*s/%s", (int) directory_size,
                 directory, program);
        if (access(candidate, X_OK) == 0) {
            resolved = realpath(candidate, NULL);
            if (resolved != NULL) {
                free(candidate);
                return resolved;
            }
            return candidate;
        }
        free(candidate);
        if (end == NULL)
            break;
        cursor = end + 1;
    }
    return strdup(program);
}

static int
configure_bundled_gl(const char *launcher_path)
{
#if XMIN_BUILD_CLIENT_GL
#if defined(__APPLE__)
    static const char variable[] = "DYLD_LIBRARY_PATH";
#else
    static const char variable[] = "LD_LIBRARY_PATH";
#endif
    const char *slash = strrchr(launcher_path, '/');
    const char *existing;
    struct stat status;
    char *directory;
    char *value;
    size_t launcher_directory_size;
    size_t directory_size;
    size_t value_size;

    if (slash == NULL)
        return 0;
    launcher_directory_size = (size_t) (slash - launcher_path);
    directory_size = launcher_directory_size + sizeof("/../") - 1 +
        strlen(XMIN_INSTALL_LIBDIR) + sizeof("/xmin");
    directory = malloc(directory_size);
    if (directory == NULL)
        return -1;
    snprintf(directory, directory_size, "%.*s/../%s/xmin",
             (int) launcher_directory_size, launcher_path,
             XMIN_INSTALL_LIBDIR);
    if (stat(directory, &status) != 0 || !S_ISDIR(status.st_mode)) {
        free(directory);
        return 0;
    }
    existing = getenv(variable);
    value_size = strlen(directory) +
        (existing != NULL && existing[0] != '\0' ? strlen(existing) + 1 : 0) +
        1;
    value = malloc(value_size);
    if (value == NULL) {
        free(directory);
        return -1;
    }
    if (existing != NULL && existing[0] != '\0')
        snprintf(value, value_size, "%s:%s", directory, existing);
    else
        snprintf(value, value_size, "%s", directory);
    free(directory);
    if (setenv(variable, value, 1) != 0) {
        free(value);
        return -1;
    }
    free(value);
#else
    (void) launcher_path;
#endif
    return 0;
}

static int
read_display_number(int fd, char *display, size_t capacity)
{
    struct pollfd ready = { .fd = fd, .events = POLLIN | POLLHUP };
    size_t used = 0;
    int poll_result;

    do {
        poll_result = poll(&ready, 1, 15000);
    } while (poll_result < 0 && errno == EINTR && pending_signal == 0);
    if (poll_result <= 0 || pending_signal != 0)
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
terminate_and_wait(pid_t process, int *status)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    int i;

    if (kill(process, SIGTERM) != 0 && errno != ESRCH)
        return -1;
    for (i = 0; i < 500; ++i) {
        pid_t result = waitpid(process, status, WNOHANG);

        if (result == process)
            return 0;
        if (result < 0 && errno != EINTR)
            return errno == ECHILD ? 0 : -1;
        nanosleep(&delay, NULL);
    }
    kill(process, SIGKILL);
    while (waitpid(process, status, 0) < 0) {
        if (errno != EINTR)
            return errno == ECHILD ? 0 : -1;
    }
    return -1;
}

static int
status_to_exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 125;
}

int
main(int argc, char **argv)
{
    const char *server_override = NULL;
    const char *screen = NULL;
    const char *dpi = NULL;
    const char *temporary_base;
    char *server_path = NULL;
    char *launcher_path = NULL;
    char **server_arguments = NULL;
    char temporary_directory[1024] = { 0 };
    char authority_path[1100] = { 0 };
    char display_fd[24];
    char display_number[8];
    char display_environment[10];
    unsigned char cookie[XMIN_COOKIE_SIZE];
    int ready_pipe[2] = { -1, -1 };
    int command_index = -1;
    int command_status = 125 << 8;
    int server_status = 0;
    int server_done = 0;
    int result = 125;
    int i;
    int argument_count;
    pid_t server = -1;
    pid_t command = -1;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            command_index = i + 1;
            break;
        }
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("xmin-run %s\n", XMIN_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            server_override = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            screen = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--dpi") == 0 && i + 1 < argc) {
            dpi = argv[++i];
            continue;
        }
        fprintf(stderr, "%s: unknown or incomplete option: %s\n",
                argv[0], argv[i]);
        print_usage(stderr, argv[0]);
        return 2;
    }
    if (command_index < 0 || command_index >= argc) {
        print_usage(stderr, argv[0]);
        return 2;
    }

    launcher_path = resolve_launcher_path(argv[0]);
    server_path = server_override != NULL ? strdup(server_override) :
        default_server_path(launcher_path != NULL ? launcher_path : argv[0]);
    if (launcher_path == NULL || server_path == NULL)
        goto cleanup;

    temporary_base = getenv("TMPDIR");
    if (temporary_base == NULL || temporary_base[0] == '\0')
        temporary_base = "/tmp";
    if (snprintf(temporary_directory, sizeof(temporary_directory),
                 "%s/xmin-run-XXXXXX", temporary_base) >=
        (int) sizeof(temporary_directory) ||
        mkdtemp(temporary_directory) == NULL) {
        perror("xmin-run: cannot create private temporary directory");
        goto cleanup;
    }
    if (chmod(temporary_directory, 0700) != 0 ||
        snprintf(authority_path, sizeof(authority_path), "%s/Xauthority",
                 temporary_directory) >= (int) sizeof(authority_path)) {
        perror("xmin-run: cannot prepare authority path");
        goto cleanup;
    }
    if (xmin_random_bytes(cookie, sizeof(cookie)) != 0 ||
        xmin_write_authority(authority_path, cookie, sizeof(cookie)) != 0) {
        perror("xmin-run: cannot create authority file");
        goto cleanup;
    }
    memset(cookie, 0, sizeof(cookie));

    if (pipe(ready_pipe) != 0 || install_signal_handlers() != 0) {
        perror("xmin-run: process setup");
        goto cleanup;
    }
    server_arguments = calloc(16, sizeof(*server_arguments));
    if (server_arguments == NULL)
        goto cleanup;
    snprintf(display_fd, sizeof(display_fd), "%d", ready_pipe[1]);
    argument_count = 0;
    server_arguments[argument_count++] = server_path;
    server_arguments[argument_count++] = (char *) "-displayfd";
    server_arguments[argument_count++] = display_fd;
    server_arguments[argument_count++] = (char *) "-auth";
    server_arguments[argument_count++] = authority_path;
    server_arguments[argument_count++] = (char *) "-noreset";
#if XMIN_ENABLE_TCP
    server_arguments[argument_count++] = (char *) "-nolisten";
    server_arguments[argument_count++] = (char *) "tcp";
#endif
    if (screen != NULL) {
        server_arguments[argument_count++] = (char *) "-screen";
        server_arguments[argument_count++] = (char *) "0";
        server_arguments[argument_count++] = (char *) screen;
    }
    if (dpi != NULL) {
        server_arguments[argument_count++] = (char *) "-dpi";
        server_arguments[argument_count++] = (char *) dpi;
    }
    server_arguments[argument_count] = NULL;

    server = fork();
    if (server < 0) {
        perror("xmin-run: cannot fork Xmin");
        goto cleanup;
    }
    if (server == 0) {
        close(ready_pipe[0]);
        reset_signal_handlers();
        execvp(server_path, server_arguments);
        perror("xmin-run: cannot execute Xmin");
        _exit(127);
    }
    close(ready_pipe[1]);
    ready_pipe[1] = -1;
    if (read_display_number(ready_pipe[0], display_number,
                            sizeof(display_number)) != 0) {
        fprintf(stderr, "xmin-run: Xmin did not report a ready display\n");
        goto cleanup;
    }
    close(ready_pipe[0]);
    ready_pipe[0] = -1;

    if (snprintf(display_environment, sizeof(display_environment), ":%s",
                 display_number) >= (int) sizeof(display_environment) ||
        setenv("DISPLAY", display_environment, 1) != 0 ||
        setenv("XAUTHORITY", authority_path, 1) != 0 ||
        configure_bundled_gl(launcher_path) != 0) {
        perror("xmin-run: cannot set child environment");
        goto cleanup;
    }

    command = fork();
    if (command < 0) {
        perror("xmin-run: cannot fork command");
        goto cleanup;
    }
    if (command == 0) {
        reset_signal_handlers();
        execvp(argv[command_index], &argv[command_index]);
        perror("xmin-run: cannot execute command");
        _exit(127);
    }

    while (1) {
        const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
        pid_t wait_result;

        if (pending_signal != 0) {
            kill(command, pending_signal);
            kill(server, pending_signal);
        }
        wait_result = waitpid(command, &command_status, WNOHANG);
        if (wait_result == command) {
            command = -1;
            break;
        }
        if (wait_result < 0 && errno != EINTR) {
            perror("xmin-run: cannot wait for command");
            goto cleanup;
        }
        wait_result = waitpid(server, &server_status, WNOHANG);
        if (wait_result == server) {
            server = -1;
            server_done = 1;
            fprintf(stderr, "xmin-run: Xmin exited before the command\n");
            if (terminate_and_wait(command, &command_status) != 0)
                fprintf(stderr, "xmin-run: command did not terminate cleanly\n");
            command = -1;
            goto cleanup;
        }
        if (wait_result < 0 && errno != EINTR) {
            perror("xmin-run: cannot wait for Xmin");
            goto cleanup;
        }
        nanosleep(&delay, NULL);
    }

    result = status_to_exit_code(command_status);

cleanup:
    if (ready_pipe[0] >= 0)
        close(ready_pipe[0]);
    if (ready_pipe[1] >= 0)
        close(ready_pipe[1]);
    if (command > 0)
        terminate_and_wait(command, &command_status);
    if (server > 0 && terminate_and_wait(server, &server_status) != 0) {
        fprintf(stderr, "xmin-run: Xmin did not terminate cleanly\n");
        result = 125;
    }
    if (server_done && result == 0)
        result = 125;
    memset(cookie, 0, sizeof(cookie));
    if (authority_path[0] != '\0' && unlink(authority_path) != 0 &&
        errno != ENOENT) {
        perror("xmin-run: cannot remove authority file");
        result = 125;
    }
    if (temporary_directory[0] != '\0' &&
        rmdir(temporary_directory) != 0 && errno != ENOENT) {
        perror("xmin-run: cannot remove temporary directory");
        result = 125;
    }
    free(server_arguments);
    free(server_path);
    free(launcher_path);
    return result;
}
