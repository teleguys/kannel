#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h>

#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

static char *progname;
static char **box_arglist;
static int min_restart_delay = 60; /* in seconds */
static pid_t child_box; /* used in main_loop, available to signal handlers */

/* Extra arguments to pass to the box */
const static char *extra_arguments[] = {
	"-v", "4",   /* Minimal output on stderr, goes to /dev/null anyway */
};
#define NUM_EXTRA (sizeof(extra_arguments) / sizeof(*extra_arguments))

void print_usage(FILE *stream)
{
	fprintf(stderr,
		"Usage: %s [--pidfile PIDFILE] [--min-delay SECONDS] BOXPATH [boxoptions...]\n",
		progname);
}

/* Create the argument list to pass to the box process, and put it
 * in the box_arglist global variable.  This is also the right place
 * to add any standard arguments that we want to pass. */
void build_box_arglist(char *boxfile, int argc, char **argv)
{
	int i;
	char **argp;

	if (box_arglist) {
		free(box_arglist);
	}

	/* one for the boxfile name itself, one for each extra argument,
	 * one for each normal argument, and one for the terminating NULL */
	box_arglist = malloc((1 + NUM_EXTRA + argc + 1) * sizeof(*box_arglist));
	if (!box_arglist) {
		fprintf(stderr, "%s: malloc: %s\n", progname, strerror(errno));
	}

	/* Have argp walk down box_arglist and set each argument. */
	argp = box_arglist;

	*argp++ = boxfile;
	for (i = 0; i < NUM_EXTRA; i++) {
		*argp++ = extra_arguments[i];
	}
	for (i = 0; i < argc; i++) {
		*argp++ = argv[i];
	}
	*argp++ = (char *)NULL;
}

void write_pidfile(char *pidfile)
{
	int fd;
	pid_t pid;
	FILE *f;

	fd = open(pidfile, O_WRONLY|O_NOCTTY|O_TRUNC|O_CREAT, 0644);
	if (fd < 0) {
		fprintf(stderr, "%s: open: %s: %s\n", progname, pidfile, strerror(errno));
		exit(1);
	}

	f = fdopen(fd, "w");
	if (!f) {
		fprintf(stderr, "%s: fdopen: %s\n", progname, strerror(errno));
		exit(1);
	}

	fprintf(f, "%ld\n", (long)getpid());
	if (fclose(f) < 0) {
		fprintf(stderr, "%s: writing %s: %s\n", progname, pidfile, strerror(errno));
		exit(1);
	}
}

/* Set 0 (stdin) to /dev/null, and 1 and 2 (stdout and stderr) to /dev/full
 * if it's available and /dev/null otherwise. */
void rebind_standard_streams(void)
{
	int devnullfd;
	int devfullfd;

	devnullfd = open("/dev/null", O_RDONLY);
	if (devnullfd < 0) {
		fprintf(stderr, "%s: cannot open /dev/null: %s\n",
			progname, strerror(errno));
		exit(2);
	}
	devfullfd = open("/dev/full", O_WRONLY);
	if (devfullfd < 0) {
		devfullfd = devnullfd;
	}

	/* Alert: The dup on stderr is done last, so that the error message
	 * works regardless of which dup fails. */
	if (dup2(devnullfd, 0) < 0 ||
	    dup2(devfullfd, 1) < 0 ||
	    dup2(devfullfd, 2) < 0) {
		fprintf(stderr, "%s: dup2: %s\n", progname, strerror(errno));
		exit(1);
	}
}

/* Some code to determine the highest possible file descriptor number,
 * so that we can close them all.  */
int open_max(void)
{
#ifdef OPEN_MAX
	return OPEN_MAX;
#else
	int max;

	max = sysconf(_SC_OPEN_MAX);
	if (max <= 0) {
		return 1024;  /* guess */
	}
	return max;
#endif
}

/* Close all file descriptors other than 0, 1, and 2. */
void close_extra_files(void)
{
	int max = open_max();
	int fd;

	for (fd = 3; fd < max; fd++) {
		close(fd);
	}
}

/* We received a signal that we should pass on to the child box.
 * We ignore it ourselves. */
void signal_transfer(int signum)
{
	if (child_box > 0) {
		kill(child_box, signum);
	}
}

/* We received a signal that we should pass on to the child box,
 * and then die from ourselves.  It has to be a signal that
 * terminates the process as its default action! */
void signal_transfer_and_die(int signum)
{
	/* First send it to the child process */
	if (child_box > 0) {
		kill(child_box, signum);
	}

	/* Then send it to self.  First set the default handler, to
	 * avoid catching the signal with this handler again.  This
	 * is not a race, because it doesn't matter if we die from
	 * the signal we're going to send or from a different one.  */
	signal(signum, SIG_DFL);
	kill(getpid(), signum);
}

void setup_signals(void)
{
	signal(SIGHUP, &signal_transfer);
	signal(SIGINT, &signal_transfer_and_die);
	signal(SIGQUIT, &signal_transfer_and_die);
	signal(SIGTERM, &signal_transfer_and_die);
	signal(SIGUSR1, &signal_transfer);
	signal(SIGUSR2, &signal_transfer);
}

/* Fork off a box process and loop indefinitely, forking a new one
 * every time it dies. */
void main_loop(char *boxfile)
{
	time_t next_fork = 0;

	/* We can't report any errors here, because we are running
	 * as a daemon and we have no logfile of our own.  So we
	 * exit with errno as the exit code, to offer a minimal clue. */

	for (;;) {

		/* Make sure we don't fork in an endless loop if something
		 * is drastically wrong.  This code limits it to one
		 * per minute. */
		time_t this_time = time(NULL);
		if (this_time <= next_fork) {
			sleep(next_fork - this_time);
		}
		next_fork = this_time + min_restart_delay;

		child_box = fork();
		if (child_box < 0) {
			exit(errno);
		}
		if (child_box == 0) {
			/* child.  exec the box */
			execvp(boxfile, box_arglist);
			exit(127);
		}

		while (waitpid(child_box, (int *)NULL, 0) != child_box) {
			if (errno == ECHILD) {
				/* Something went wrong... we don't know what,
				 * but we do know that our child does not
				 * exist.  So restart it. */
				break;
			}
			if (errno == EINTR) {
				continue;
			}
			/* Something weird happened. */
			exit(errno);
		}
	}
}

int main(int argc, char *argv[])
{
	int i;
	char *pidfile = NULL;
	char *boxfile = NULL;
	pid_t childpid;

	progname = argv[0];

	if (argc == 1) {
		print_usage(stderr);
		exit(2);
	}

	/* Parse the options meant for the wrapper, and get the name of
	 * the box to wrap. */
	for (i = 1; i < argc && !boxfile; i++) {
		if (strcmp(argv[i], "--pidfile") == 0) {
			if (i+1 >= argc) {
				fprintf(stderr, "Missing argument for option %s\n", argv[i]);
				exit(2);
			}
			pidfile = argv[i+1];
			i++;
		} else if (strcmp(argv[i], "--min-delay") == 0) {
			if (i+1 >= argc) {
				fprintf(stderr, "Missing argument for option %s", argv[i]);
				exit(2);
			}
			min_restart_delay = atoi(argv[i+1]);
			i++;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "Unknown option %s\n", argv[i]);
			exit(2);
		} else {
			boxfile = argv[i];
		}
	}

	/* Check if we have everything */
	if (!boxfile) {
		print_usage(stderr);
		exit(2);
	}

	/* The remaining arguments should be passed to the box */
	build_box_arglist(boxfile, argc - i, argv + i);

	/* Ready to rock.  Begin daemonization. */

	/* Fork a child process and have the parent exit.
         * This makes us run in the background. */
	childpid = fork();
	if (childpid < 0) {
		fprintf(stderr, "%s: fork: %s\n", progname, strerror(errno));
		exit(1);
	}
	if (childpid != 0) {
		exit(0); /* parent exits immediately */
	}
	
	/* The child continues here.  Now call setsid() to disconnect
	 * from our terminal and from the parent's session and process
	 * group. */
	if (setsid() < 0) {
		fprintf(stderr, "%s: setsid: %s\n", progname, strerror(errno));
		exit(1);
	}

	/* Change to the root directory, so that we don't keep a
	 * file descriptor open on an unknown directory. */
	if (chdir("/") < 0) {
		fprintf(stderr, "%s: chdir to root: %s\n", progname, strerror(errno));
		exit(1);
	}

	if (pidfile) {
		write_pidfile(pidfile);
	}

	/* Set the umask to a known value, rather than inheriting
	 * an unknown one. */
	umask(077);

	/* Leave file descriptors 0, 1, and 2 pointing to harmless
	 * places, and close all other file descriptors. */
	rebind_standard_streams();
	close_extra_files();

	setup_signals();
	main_loop(boxfile);
}
