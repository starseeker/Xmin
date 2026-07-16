#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    BATCH_SIZE = 8,
    ROUNDS = 3
};

static int
write_all(int fd, const void *buffer, size_t size)
{
    const char *cursor = buffer;

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
run_round(char **argv)
{
    static const char release[BATCH_SIZE] = { 0 };
    pid_t children[BATCH_SIZE];
    int gate[2];
    int launched = 0;
    int failed = 0;
    int i;

    if (pipe(gate) != 0)
        return -1;
    for (i = 0; i < BATCH_SIZE; ++i) {
        pid_t child = fork();

        if (child < 0) {
            failed = 1;
            break;
        }
        if (child == 0) {
            char token;
            ssize_t count;

            close(gate[1]);
            do {
                count = read(gate[0], &token, 1);
            } while (count < 0 && errno == EINTR);
            close(gate[0]);
            if (count != 1)
                _exit(126);
            execl(argv[1], argv[1], "--server", argv[2],
                  "--screen", "160x120x24", "--", argv[3],
                  (char *) NULL);
            _exit(127);
        }
        children[launched++] = child;
    }

    close(gate[0]);
    if (write_all(gate[1], release, (size_t) launched) != 0)
        failed = 1;
    close(gate[1]);

    for (i = 0; i < launched; ++i) {
        int status;
        pid_t waited;

        do {
            waited = waitpid(children[i], &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited != children[i] || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            failed = 1;
    }
    return failed ? -1 : 0;
}

int
main(int argc, char **argv)
{
    int round;

    if (argc != 4) {
        fprintf(stderr, "usage: %s xmin-run Xmin launcher-child\n", argv[0]);
        return 2;
    }
    for (round = 0; round < ROUNDS; ++round) {
        if (run_round(argv) != 0) {
            fprintf(stderr, "concurrent launcher round %d failed\n", round + 1);
            return 1;
        }
    }
    return 0;
}
