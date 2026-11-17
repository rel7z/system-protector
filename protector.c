/*
 * system-protector  --  a tiny, dependency-free process watchdog for Linux.
 *
 * It launches a command you give it, watches it, and re-launches it whenever
 * it exits or is killed (SIGKILL included). Restarts use exponential backoff
 * so a program that crash-loops doesn't burn the CPU, but a program that was
 * killed after running fine comes straight back.
 *
 * It intentionally does NOT protect itself from being killed -- nothing on
 * Linux can make a process un-killable by root. For "even if the watchdog is
 * killed" resilience, run this under systemd (Restart=always). Then:
 *   - the watchdog respawns your app   (sub-second, no root needed)
 *   - systemd (PID 1) respawns the watchdog if the watchdog itself is killed
 *
 * Build:  cc -O2 -Wall -Wextra -o protector protector.c
 * Usage:  protector [options] -- <command> [args...]
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* ---- configuration (filled from argv) ---- */
static const char *opt_name      = "protected";
static const char *opt_pidfile   = NULL;
static const char *opt_logfile   = NULL;
static int         opt_daemon    = 0;
static double      opt_min_backoff = 1.0;   /* seconds */
static double      opt_max_backoff = 30.0;  /* seconds */
static double      opt_backoff_factor = 2.0;
static double      opt_healthy_secs = 15.0; /* ran at least this long -> reset backoff */
static char      **child_argv    = NULL;    /* points into argv */

/* ---- runtime state ---- */
static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t got_sigchld    = 0;
static pid_t child_pid = 0;
static FILE *logfp = NULL;

static void log_msg(const char *fmt, ...) {
    FILE *out = logfp ? logfp : stderr;
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    fprintf(out, "%s [system-protector:%s] ", ts, opt_name);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}

static void on_stop(int sig) {
    stop_requested = 1;
    if (child_pid > 0) {
        /* forward the signal to the child's whole process group */
        kill(-child_pid, sig);
    }
}

static void on_sigchld(int sig) {
    (void)sig;
    got_sigchld = 1; /* only used to interrupt nanosleep early */
}

/* Sleep up to `secs`, but wake immediately on stop or child-exit. */
static void interruptible_sleep(double secs) {
    struct timespec req;
    req.tv_sec  = (time_t)secs;
    req.tv_nsec = (long)((secs - (double)req.tv_sec) * 1e9);
    while (req.tv_sec > 0 || req.tv_nsec > 0) {
        if (stop_requested) return;
        struct timespec rem;
        if (nanosleep(&req, &rem) == 0) return;
        if (errno == EINTR) {
            if (stop_requested) return;
            req = rem;            /* resume remaining time */
            continue;
        }
        return;
    }
}

static void write_pidfile(void) {
    if (!opt_pidfile) return;
    FILE *f = fopen(opt_pidfile, "w");
    if (!f) {
        log_msg("warning: cannot write pidfile %s: %s", opt_pidfile, strerror(errno));
        return;
    }
    fprintf(f, "%ld\n", (long)getpid());
    fclose(f);
}

static void remove_pidfile(void) {
    if (opt_pidfile) unlink(opt_pidfile);
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);          /* parent leaves */
    if (setsid() < 0) { perror("setsid"); exit(1); }
    pid = fork();                  /* second fork: never a session leader */
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);
    if (chdir("/") < 0) { /* ignore */ }
    umask(0);
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        if (!logfp) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); }
        if (fd > 2) close(fd);
    }
}

/* Launch the child in its own process group. Returns pid or -1. */
static pid_t spawn_child(void) {
    pid_t pid = fork();
    if (pid < 0) {
        log_msg("fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* ---- child ---- */
        setpgid(0, 0);  /* own process group so we can signal the whole tree */

        /* restore default signal handling in the child */
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        if (opt_logfile) {
            int fd = open(opt_logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > 2) close(fd);
            }
        }
        execvp(child_argv[0], child_argv);
        /* only reached if exec failed */
        fprintf(stderr, "system-protector: exec %s failed: %s\n",
                child_argv[0], strerror(errno));
        _exit(127);
    }
    /* ---- parent ---- */
    setpgid(pid, pid); /* race-free: set it on both sides */
    return pid;
}

static void usage(const char *prog) {
    fprintf(stderr,
"Usage: %s [options] -- <command> [args...]\n"
"\n"
"Keeps <command> running: respawns it whenever it exits or is killed.\n"
"\n"
"Options:\n"
"  --name NAME          label for logs and systemd (default: protected)\n"
"  --pidfile PATH       write the watchdog's own PID here\n"
"  --logfile PATH       append watchdog + child output here\n"
"  --min-backoff SECS   first restart delay (default: 1)\n"
"  --max-backoff SECS   cap on restart delay (default: 30)\n"
"  --healthy SECS       child must run this long to reset backoff (default: 15)\n"
"  --daemon             detach into the background\n"
"  -h, --help           show this help\n"
"\n"
"Example:\n"
"  %s --name myapp --logfile /var/log/myapp.log -- python3 /opt/myapp/app.py\n",
        prog, prog);
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) opt_name = argv[++i];
        else if (strcmp(argv[i], "--pidfile") == 0 && i + 1 < argc) opt_pidfile = argv[++i];
        else if (strcmp(argv[i], "--logfile") == 0 && i + 1 < argc) opt_logfile = argv[++i];
        else if (strcmp(argv[i], "--min-backoff") == 0 && i + 1 < argc) opt_min_backoff = atof(argv[++i]);
        else if (strcmp(argv[i], "--max-backoff") == 0 && i + 1 < argc) opt_max_backoff = atof(argv[++i]);
        else if (strcmp(argv[i], "--healthy") == 0 && i + 1 < argc) opt_healthy_secs = atof(argv[++i]);
        else if (strcmp(argv[i], "--daemon") == 0) opt_daemon = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(argv[0]); return 2; }
    }

    if (i >= argc) {
        fprintf(stderr, "error: no command given (use -- <command> [args...])\n\n");
        usage(argv[0]);
        return 2;
    }
    child_argv = &argv[i];

    if (opt_min_backoff < 0.05) opt_min_backoff = 0.05;
    if (opt_max_backoff < opt_min_backoff) opt_max_backoff = opt_min_backoff;

    if (opt_logfile) {
        logfp = fopen(opt_logfile, "a");
        if (!logfp) {
            fprintf(stderr, "warning: cannot open logfile %s: %s\n",
                    opt_logfile, strerror(errno));
        }
    }

    if (opt_daemon) daemonize();

    /* install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    write_pidfile();
    log_msg("watchdog started (pid %ld), protecting: %s", (long)getpid(), child_argv[0]);

    double backoff = opt_min_backoff;

    while (!stop_requested) {
        time_t started = time(NULL);
        child_pid = spawn_child();
        if (child_pid < 0) {
            interruptible_sleep(backoff);
            backoff = backoff * opt_backoff_factor;
            if (backoff > opt_max_backoff) backoff = opt_max_backoff;
            continue;
        }
        log_msg("started child pid %ld", (long)child_pid);

        /* wait for this specific child */
        int status = 0;
        pid_t w;
        do {
            w = waitpid(child_pid, &status, 0);
        } while (w < 0 && errno == EINTR && !stop_requested);

        long ran = (long)(time(NULL) - started);

        if (stop_requested) {
            /* make sure the child is gone */
            if (child_pid > 0) {
                kill(-child_pid, SIGTERM);
                interruptible_sleep(5.0);
                kill(-child_pid, SIGKILL);
                waitpid(child_pid, &status, 0);
            }
            break;
        }

        if (WIFEXITED(status)) {
            log_msg("child exited with code %d after %lds", WEXITSTATUS(status), ran);
        } else if (WIFSIGNALED(status)) {
            log_msg("child killed by signal %d after %lds", WTERMSIG(status), ran);
        } else {
            log_msg("child ended (status %d) after %lds", status, ran);
        }

        if ((double)ran >= opt_healthy_secs) {
            /* It ran fine, so this was not a crash loop (e.g. it was killed, or
             * finished a normal run). Respawn immediately, with no delay. */
            backoff = opt_min_backoff;
            log_msg("restarting immediately");
        } else {
            /* It died almost at once -- back off so a genuine crash loop can't
             * peg the CPU. The delay grows until the program stays up again. */
            log_msg("restarting in %.2fs", backoff);
            interruptible_sleep(backoff);
            backoff = backoff * opt_backoff_factor;
            if (backoff > opt_max_backoff) backoff = opt_max_backoff;
        }
    }

    log_msg("watchdog stopping");
    remove_pidfile();
    if (logfp) fclose(logfp);
    return 0;
}
