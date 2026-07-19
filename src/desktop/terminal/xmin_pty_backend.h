#ifndef XMIN_PTY_BACKEND_H
#define XMIN_PTY_BACKEND_H

#include <stddef.h>
#include <sys/types.h>

struct xmin_pty_options {
	const char *line;
	char *command;
	const char *output;
	char **arguments;
	char *shell;
	char *scroll;
	char *utmp;
	char *stty_args;
	char *termname;
};

int xmin_pty_open(const struct xmin_pty_options *);
ssize_t xmin_pty_read(void *, size_t);
ssize_t xmin_pty_write(const void *, size_t);
void xmin_pty_resize(int, int, int, int);
void xmin_pty_hangup(void);
void xmin_pty_send_break(void);
void xmin_pty_print(const void *, size_t);

#endif
