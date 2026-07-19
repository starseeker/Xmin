/* Unix process/PTY backend for xmin-st.  The terminal core is backend-neutral. */
#include "xmin_pty_backend.h"

#include "st.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
# include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
# include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
# include <libutil.h>
#endif

static int master_fd = -1;
static int output_fd = 1;
static pid_t child_pid = -1;

static void
child_signal(int unused)
{
	int saved_errno = errno;
	int status;
	pid_t result;
	(void)unused;
	result = waitpid(child_pid, &status, WNOHANG);
	if (result == child_pid) {
		if (WIFEXITED(status))
			_exit(WEXITSTATUS(status));
		if (WIFSIGNALED(status))
			_exit(128 + WTERMSIG(status));
		_exit(1);
	}
	errno = saved_errno;
}

static void
configure_line(char *const *arguments, const char *defaults)
{
	char command[_POSIX_ARG_MAX];
	char *cursor = command;
	size_t remaining = sizeof(command);
	int written = snprintf(cursor, remaining, "%s", defaults ? defaults : "");
	if (written < 0 || (size_t)written >= remaining)
		die("incorrect stty parameters\n");
	cursor += written;
	remaining -= (size_t)written;
	for (; arguments && *arguments; ++arguments) {
		written = snprintf(cursor, remaining, " %s", *arguments);
		if (written < 0 || (size_t)written >= remaining)
			die("stty parameter length too long\n");
		cursor += written;
		remaining -= (size_t)written;
	}
	if (system(command) != 0)
		perror("Couldn't call stty");
}

static void
exec_shell(const struct xmin_pty_options *options)
{
	const struct passwd *password;
	char *login_shell;
	char *program;
	char *argument;
	char **arguments = options->arguments;

	errno = 0;
	password = getpwuid(getuid());
	if (!password)
		die(errno ? "getpwuid: %s\n" : "unable to identify user\n",
		    errno ? strerror(errno) : "");
	login_shell = getenv("XMIN_TERMINAL_SHELL");
	if (!login_shell || !*login_shell)
		login_shell = getenv("SHELL");
	if (!login_shell || !*login_shell)
		login_shell = password->pw_shell[0] ? password->pw_shell : options->shell;
	if (arguments) {
		program = arguments[0];
		argument = NULL;
	} else if (options->scroll) {
		program = options->scroll;
		argument = options->utmp ? options->utmp : login_shell;
	} else if (options->utmp) {
		program = options->utmp;
		argument = NULL;
	} else {
		program = login_shell;
		argument = NULL;
	}
	if (!arguments) {
		static char *fallback[3];
		fallback[0] = program;
		fallback[1] = argument;
		fallback[2] = NULL;
		arguments = fallback;
	}

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");
	setenv("LOGNAME", password->pw_name, 1);
	setenv("USER", password->pw_name, 1);
	setenv("SHELL", login_shell, 1);
	setenv("HOME", password->pw_dir, 1);
	setenv("TERM", options->termname, 1);
	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);
	execvp(program, arguments);
	_exit(1);
}

int
xmin_pty_open(const struct xmin_pty_options *options)
{
	struct sigaction child_action;
	int slave;
	if (!options)
		return -1;
	if (options->output) {
		output_fd = strcmp(options->output, "-") == 0 ? 1 :
			open(options->output, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (output_fd < 0)
			fprintf(stderr, "Error opening %s: %s\n", options->output,
			        strerror(errno));
	}
	if (options->line) {
		master_fd = open(options->line, O_RDWR);
		if (master_fd < 0)
			die("open line '%s' failed: %s\n", options->line, strerror(errno));
		dup2(master_fd, 0);
		configure_line(options->arguments, options->stty_args);
		return master_fd;
	}
	if (openpty(&master_fd, &slave, NULL, NULL, NULL) < 0)
		die("openpty failed: %s\n", strerror(errno));
	child_pid = fork();
	if (child_pid < 0)
		die("fork failed: %s\n", strerror(errno));
	if (child_pid == 0) {
		if (output_fd > 2)
			close(output_fd);
		close(master_fd);
		setsid();
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);
		if (ioctl(slave, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
		if (slave > 2)
			close(slave);
		exec_shell(options);
	}
	close(slave);
	memset(&child_action, 0, sizeof(child_action));
	child_action.sa_handler = child_signal;
	sigemptyset(&child_action.sa_mask);
	child_action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	if (sigaction(SIGCHLD, &child_action, NULL) < 0)
		die("installing SIGCHLD handler failed: %s\n", strerror(errno));
	return master_fd;
}

ssize_t
xmin_pty_read(void *buffer, size_t capacity)
{
	return read(master_fd, buffer, capacity);
}

ssize_t
xmin_pty_write(const void *buffer, size_t length)
{
	const char *cursor = buffer;
	size_t total = 0;
	while (total < length) {
		ssize_t written = write(master_fd, cursor + total, length - total);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return written;
		total += (size_t)written;
	}
	return (ssize_t)total;
}

void
xmin_pty_resize(int rows, int columns, int width, int height)
{
	struct winsize size = {
		.ws_row = (unsigned short)rows,
		.ws_col = (unsigned short)columns,
		.ws_xpixel = (unsigned short)width,
		.ws_ypixel = (unsigned short)height,
	};
	if (ioctl(master_fd, TIOCSWINSZ, &size) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

void
xmin_pty_hangup(void)
{
	if (child_pid > 0)
		kill(child_pid, SIGHUP);
}

void
xmin_pty_send_break(void)
{
	if (tcsendbreak(master_fd, 0) != 0)
		perror("Error sending break");
}

void
xmin_pty_print(const void *buffer, size_t length)
{
	const char *cursor = buffer;
	size_t total = 0;
	if (output_fd < 0)
		return;
	while (total < length) {
		ssize_t written = write(output_fd, cursor + total, length - total);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			perror("Error writing terminal output");
			if (output_fd > 2)
				close(output_fd);
			output_fd = -1;
			return;
		}
		total += (size_t)written;
	}
}
