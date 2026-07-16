#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char target_name[] = "xmin-qt-input-target";

static int
run_command(const char *program, char *const arguments[])
{
    const pid_t child = fork();
    int status = 0;
    if (child < 0)
        return -1;
    if (child == 0) {
        execv(program, arguments);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int
wait_for_child(pid_t child, int *status)
{
    const struct timespec delay = { 0, 20 * 1000 * 1000 };
    for (int attempt = 0; attempt < 250; ++attempt) {
        const pid_t result = waitpid(child, status, WNOHANG);
        if (result == child)
            return 0;
        if (result < 0 && errno != EINTR)
            return -1;
        nanosleep(&delay, NULL);
    }
    return -1;
}

int
main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s xminctl qt-input-target\n", argv[0]);
        return 2;
    }
    const char *control = argv[1];
    const char *target = argv[2];
    char *wait_arguments[] = { (char *) control, "wait-window", "--timeout",
                               "5000", (char *) target_name, NULL };
    char *activate_arguments[] = { (char *) control, "activate",
                                   (char *) target_name, NULL };
    char *control_down_arguments[] = { (char *) control, "key-down", "ctrl", NULL };
    char *click_arguments[] = { (char *) control, "click", "--delay", "5",
                                (char *) target_name, "20", "20", NULL };
    char *drag_arguments[] = { (char *) control, "mouse-drag", "--steps", "3",
                               "--delay", "5", (char *) target_name,
                               "10", "10", "30", "30", NULL };
    char *control_up_arguments[] = { (char *) control, "key-up", "ctrl", NULL };
    char *key_arguments[] = { (char *) control, "type", "--delay", "5", "a", NULL };
    char *close_arguments[] = { (char *) control, "close",
                                (char *) target_name, NULL };
    const char *stage = "starting target";
    pid_t child = fork();
    int child_status = 0;
    int result = 1;

    if (child < 0)
        goto cleanup;
    if (child == 0) {
        execl(target, target, (char *) NULL);
        _exit(127);
    }
    stage = "waiting for target";
    if (run_command(control, wait_arguments) != 0)
        goto cleanup;
    stage = "activating target";
    if (run_command(control, activate_arguments) != 0)
        goto cleanup;
    stage = "injecting pointer and modifier input";
    if (run_command(control, control_down_arguments) != 0 ||
        run_command(control, click_arguments) != 0 ||
        run_command(control, drag_arguments) != 0 ||
        run_command(control, control_up_arguments) != 0)
        goto cleanup;
    stage = "injecting printable keyboard input";
    if (run_command(control, key_arguments) != 0)
        goto cleanup;
    stage = "closing target";
    if (run_command(control, close_arguments) != 0 ||
        wait_for_child(child, &child_status) != 0 ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
        goto cleanup;
    child = -1;
    result = 0;

cleanup:
    if (child > 0) {
        kill(child, SIGTERM);
        (void) waitpid(child, NULL, 0);
    }
    if (result != 0)
        fprintf(stderr, "Qt input integration failed while %s\n", stage);
    return result;
}
