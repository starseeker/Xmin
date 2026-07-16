#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char target_name[] = "xminctl-automation-target";

static int
run_command(const char *program, char *const arguments[])
{
    pid_t child = fork();
    int status;

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
read_ppm_pixel(const char *path, unsigned int expected_width,
               unsigned int expected_height, unsigned int x, unsigned int y,
               uint8_t rgb[3])
{
    char magic[3];
    unsigned int width;
    unsigned int height;
    unsigned int maximum;
    FILE *file = fopen(path, "rb");
    long offset;
    int separator;
    int result = -1;

    if (file == NULL)
        return -1;
    if (fscanf(file, "%2s%u%u%u", magic, &width, &height, &maximum) != 4 ||
        strcmp(magic, "P6") != 0 || width != expected_width ||
        height != expected_height || maximum != 255 || x >= width || y >= height)
        goto cleanup;
    separator = fgetc(file);
    if (separator == EOF)
        goto cleanup;
    offset = (long) (((size_t) y * width + x) * 3U);
    if (fseek(file, offset, SEEK_CUR) != 0 || fread(rgb, 3, 1, file) != 1)
        goto cleanup;
    result = 0;

cleanup:
    fclose(file);
    return result;
}

static int
green_pixel(const uint8_t rgb[3])
{
    return rgb[0] < 8 && rgb[1] > 247 && rgb[2] < 8;
}

static int
wait_for_child(pid_t child, int *status)
{
    struct timespec delay = { 0, 20 * 1000 * 1000 };
    int attempt;

    for (attempt = 0; attempt < 100; ++attempt) {
        pid_t result = waitpid(child, status, WNOHANG);

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
    const char *control;
    const char *target;
    char window_path[128];
    char root_path[128];
    char *wait_arguments[] = { NULL, "wait-window", "--timeout", "5000",
                               (char *) target_name, NULL };
    char *activate_arguments[] = { NULL, "activate", (char *) target_name, NULL };
    char *click_arguments[] = { NULL, "click", "--delay", "5",
                                (char *) target_name, "20", "20", NULL };
    char *drag_arguments[] = { NULL, "mouse-drag", "--steps", "3",
                               "--delay", "5", (char *) target_name,
                               "10", "10", "30", "30", NULL };
    char *control_down_arguments[] = { NULL, "key-down", "ctrl", NULL };
    char *control_up_arguments[] = { NULL, "key-up", "ctrl", NULL };
    char *key_arguments[] = { NULL, "type", "--delay", "5", "a", NULL };
    char *stable_arguments[] = { NULL, "wait-stable", "--quiet", "200",
                                 "--timeout", "3000", (char *) target_name,
                                 NULL };
    char *capture_window_arguments[] = { NULL, "capture-window",
                                         (char *) target_name, window_path,
                                         NULL };
    char *capture_root_arguments[] = { NULL, "capture-root", root_path, NULL };
    char *close_arguments[] = { NULL, "close", (char *) target_name, NULL };
    uint8_t window_pixel[3];
    uint8_t root_pixel[3];
    pid_t target_child = -1;
    int target_status = 0;
    const char *stage = "starting target";
    int result = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s xminctl target\n", argv[0]);
        return 2;
    }
    control = argv[1];
    target = argv[2];
    if (snprintf(window_path, sizeof(window_path),
                 "/tmp/xminctl-%ld-window.ppm", (long) getpid()) >=
            (int) sizeof(window_path) ||
        snprintf(root_path, sizeof(root_path),
                 "/tmp/xminctl-%ld-root.ppm", (long) getpid()) >=
            (int) sizeof(root_path))
        return 2;
    wait_arguments[0] = (char *) control;
    activate_arguments[0] = (char *) control;
    click_arguments[0] = (char *) control;
    drag_arguments[0] = (char *) control;
    control_down_arguments[0] = (char *) control;
    control_up_arguments[0] = (char *) control;
    key_arguments[0] = (char *) control;
    stable_arguments[0] = (char *) control;
    capture_window_arguments[0] = (char *) control;
    capture_root_arguments[0] = (char *) control;
    close_arguments[0] = (char *) control;

    target_child = fork();
    if (target_child < 0)
        goto cleanup;
    if (target_child == 0) {
        execl(target, target, (char *) NULL);
        _exit(127);
    }
    stage = "waiting for target window";
    if (run_command(control, wait_arguments) != 0)
        goto cleanup;
    stage = "activating target window";
    if (run_command(control, activate_arguments) != 0)
        goto cleanup;
    stage = "injecting pointer input";
    if (run_command(control, control_down_arguments) != 0 ||
        run_command(control, click_arguments) != 0 ||
        run_command(control, drag_arguments) != 0 ||
        run_command(control, control_up_arguments) != 0)
        goto cleanup;
    stage = "injecting keyboard input";
    if (run_command(control, key_arguments) != 0)
        goto cleanup;
    stage = "waiting for rendering";
    if (run_command(control, stable_arguments) != 0)
        goto cleanup;
    stage = "capturing window";
    if (run_command(control, capture_window_arguments) != 0)
        goto cleanup;
    stage = "capturing root";
    if (run_command(control, capture_root_arguments) != 0)
        goto cleanup;
    stage = "checking PPM pixels";
    if (read_ppm_pixel(window_path, 80, 60, 40, 30, window_pixel) != 0 ||
        read_ppm_pixel(root_path, 160, 120, 60, 60, root_pixel) != 0 ||
        !green_pixel(window_pixel) || !green_pixel(root_pixel))
        goto cleanup;
    stage = "closing target window";
    if (run_command(control, close_arguments) != 0 ||
        wait_for_child(target_child, &target_status) != 0 ||
        !WIFEXITED(target_status) || WEXITSTATUS(target_status) != 0)
        goto cleanup;
    target_child = -1;
    result = 0;

cleanup:
    if (target_child > 0) {
        kill(target_child, SIGTERM);
        (void) waitpid(target_child, NULL, 0);
    }
    unlink(window_path);
    unlink(root_path);
    if (result != 0)
        fprintf(stderr, "xminctl integration failed while %s\n", stage);
    return result;
}
