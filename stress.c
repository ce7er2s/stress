/*
 * stress -- repeatedly spawn a command in the background at a fixed rate
 *
 * C port of a clunky bash script I used before. Full feature parity:
 *   -i interval, -n count, -c concurrent, -e stop-on-error,
 *   -o output, -L output-lines, --color WHEN, -q quiet
 *   short-flag clustering (-eo, -eoL5), GNU-style aligned colored logging,
 *   millisecond timestamps, per-job wall + maxrss via wait4/getrusage,
 *   drift-compensated absolute-deadline scheduling.
 *
 * Portable across Linux and BSD/macOS:
 *   - clock_nanosleep(TIMER_ABSTIME) where available, nanosleep fallback on macOS
 *   - wait4() for per-child rusage
 *
 * Build:  cc -O2 -Wall -Wextra -o stress stress.c
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * platform shims
 * ------------------------------------------------------------------ */

/* ru_maxrss units differ: Linux reports kilobytes, BSD/macOS report bytes. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  define MAXRSS_IS_BYTES 1
#else
#  define MAXRSS_IS_BYTES 0
#endif

/* ------------------------------------------------------------------ *
 * options / globals
 * ------------------------------------------------------------------ */

static const char *prog = "stress";

static double interval     = 1.0;   /* seconds between launches            */
static long   count        = 0;     /* 0 = unlimited                       */
static long   max_conc     = 0;     /* 0 = unlimited                       */
static int    quiet        = 0;
static int    stop_on_err  = 0;
static int    show_output  = 0;
static long   output_lines = 10;    /* 0 = unlimited                       */
enum { COLOR_AUTO, COLOR_ALWAYS, COLOR_NEVER };
static int    color_mode   = COLOR_AUTO;

static char **cmd;                  /* NULL-terminated argv for the child  */

/* counters */
static long launched   = 0;
static long completed  = 0;
static long failed     = 0;
static long running    = 0;
static int  fail_seen  = 0;
static double sum_wall = 0.0;       /* sum of per-job wall seconds         */
static long peak_rss   = 0;         /* KB                                  */

static volatile sig_atomic_t got_sigchld = 0;
static volatile sig_atomic_t stopping     = 0;

/* ------------------------------------------------------------------ *
 * per-job bookkeeping
 *
 * A small open-addressing-free approach: a dynamic array of live jobs.
 * Job counts are bounded by max_conc in practice; linear scan is fine
 * and keeps the hot path allocation-free.
 * ------------------------------------------------------------------ */

#define LINEBUF_MAX 4096

typedef struct {
    pid_t  pid;
    long   id;
    struct timespec start;
    int    outfd;            /* read end of capture pipe, or -1            */
    /* ring of last N captured lines (only used when show_output)         */
    char **ring;             /* output_lines slots (or unlimited via grow)*/
    long   ring_cap;
    long   ring_head;        /* next write index                          */
    long   ring_count;       /* how many slots are filled                 */
    long   total_lines;      /* total lines seen (for "N more" note)      */
    char   partial[LINEBUF_MAX]; /* incomplete trailing line              */
    size_t partial_len;
    int    used;
} job_t;

static job_t *jobs;
static long   jobs_cap;

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "%s: out of memory\n", prog); exit(1); }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) { fprintf(stderr, "%s: out of memory\n", prog); exit(1); }
    return p;
}

static job_t *job_alloc(void) {
    for (long i = 0; i < jobs_cap; i++)
        if (!jobs[i].used) { memset(&jobs[i], 0, sizeof jobs[i]); jobs[i].used = 1; jobs[i].outfd = -1; return &jobs[i]; }
    long old = jobs_cap;
    jobs_cap = jobs_cap ? jobs_cap * 2 : 16;
    jobs = xrealloc(jobs, (size_t)jobs_cap * sizeof *jobs);
    memset(&jobs[old], 0, (size_t)(jobs_cap - old) * sizeof *jobs);
    jobs[old].used = 1; jobs[old].outfd = -1;
    return &jobs[old];
}

static job_t *job_find(pid_t pid) {
    for (long i = 0; i < jobs_cap; i++)
        if (jobs[i].used && jobs[i].pid == pid) return &jobs[i];
    return NULL;
}

static void job_ring_init(job_t *j) {
    if (!show_output) return;
    j->ring_cap = output_lines > 0 ? output_lines : 64;
    j->ring = xmalloc((size_t)j->ring_cap * sizeof *j->ring);
    for (long i = 0; i < j->ring_cap; i++) j->ring[i] = NULL;
}

static void job_ring_push(job_t *j, const char *line) {
    j->total_lines++;
    if (output_lines > 0) {
        /* keep only the FIRST output_lines lines (head semantics, matching
         * the bash version's `head -n`) */
        if (j->ring_count < output_lines) {
            j->ring[j->ring_count++] = strdup(line);
        }
        /* else: drop; we only show the head */
    } else {
        /* unlimited: grow */
        if (j->ring_count == j->ring_cap) {
            j->ring_cap *= 2;
            j->ring = xrealloc(j->ring, (size_t)j->ring_cap * sizeof *j->ring);
        }
        j->ring[j->ring_count++] = strdup(line);
    }
}

static void job_free(job_t *j) {
    if (j->ring) {
        for (long i = 0; i < j->ring_count; i++) free(j->ring[i]);
        free(j->ring);
    }
    if (j->outfd >= 0) close(j->outfd);
    memset(j, 0, sizeof *j);
    j->outfd = -1;
}

/* ------------------------------------------------------------------ *
 * color
 * ------------------------------------------------------------------ */

static const char *C_RESET="", *C_DIM="", *C_INFO="", *C_RUN="",
                  *C_DONE="", *C_FAIL="", *C_OUT="", *C_TOTAL="";

static void color_setup(void) {
    int use = 0;
    switch (color_mode) {
        case COLOR_ALWAYS: use = 1; break;
        case COLOR_NEVER:  use = 0; break;
        case COLOR_AUTO:   use = isatty(STDOUT_FILENO) && !getenv("NO_COLOR"); break;
    }
    if (!use) return;
    C_RESET="\033[0m"; C_DIM="\033[2m";   C_INFO="\033[34m"; C_RUN="\033[36m";
    C_DONE="\033[32m"; C_FAIL="\033[1;31m"; C_OUT="\033[35m"; C_TOTAL="\033[1m";
}

static const char *tag_color(const char *tag) {
    if (!strcmp(tag,"info"))  return C_INFO;
    if (!strcmp(tag,"run"))   return C_RUN;
    if (!strcmp(tag,"done"))  return C_DONE;
    if (!strcmp(tag,"fail"))  return C_FAIL;
    if (!strcmp(tag,"out"))   return C_OUT;
    if (!strcmp(tag,"total")) return C_TOTAL;
    return C_RESET;
}

#define TAG_W 5
#define ID_W  5

/* ------------------------------------------------------------------ *
 * time helpers
 * ------------------------------------------------------------------ */

static void mono(struct timespec *ts) { clock_gettime(CLOCK_MONOTONIC, ts); }

static double ts_diff(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec)
         + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

/* wall-clock HH:MM:SS.mmm into buf */
static void timestamp(char *buf, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int ms = (int)(ts.tv_nsec / 1000000);
    snprintf(buf, n, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

/* ------------------------------------------------------------------ *
 * logging -- aligned, colorized, padding inside the brackets
 * ------------------------------------------------------------------ */

static void vlog(const char *tag, const char *fmt, va_list ap) {
    if (quiet && !strcmp(tag, "run")) return;
    char tsbuf[32];
    timestamp(tsbuf, sizeof tsbuf);
    /* "%s%s%s [%s%-*s%s] " : dim ts reset  [ color tag reset ] */
    printf("%s%s%s [%s%-*s%s] ",
           C_DIM, tsbuf, C_RESET,
           tag_color(tag), TAG_W, tag, C_RESET);
    vprintf(fmt, ap);
    putchar('\n');
}

static void slog(const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog(tag, fmt, ap); va_end(ap);
}

/* print a finished job's captured output block */
static void emit_output(job_t *j) {
    if (!show_output || j->ring_count == 0) return;
    char tsbuf[32];
    timestamp(tsbuf, sizeof tsbuf);
    printf("%s%s%s [%s%-*s%s] job %ld output:\n",
           C_DIM, tsbuf, C_RESET, tag_color("out"), TAG_W, "out", C_RESET, j->id);
    for (long i = 0; i < j->ring_count; i++)
        printf("    %s|%s %s\n", C_DIM, C_RESET, j->ring[i]);
    if (output_lines > 0 && j->total_lines > output_lines)
        printf("    %s| ... (%ld more lines)%s\n",
               C_DIM, j->total_lines - output_lines, C_RESET);
}

/* ------------------------------------------------------------------ *
 * capture pipe draining
 * ------------------------------------------------------------------ */

/* read whatever is available on j->outfd, split into lines into the ring.
 * returns 0 on EOF (pipe closed), 1 if more may come, -1 on hard error. */
static int drain_job(job_t *j) {
    char buf[8192];
    for (;;) {
        ssize_t r = read(j->outfd, buf, sizeof buf);
        if (r > 0) {
            for (ssize_t i = 0; i < r; i++) {
                char ch = buf[i];
                if (ch == '\n') {
                    j->partial[j->partial_len] = '\0';
                    job_ring_push(j, j->partial);
                    j->partial_len = 0;
                } else if (j->partial_len < LINEBUF_MAX - 1) {
                    j->partial[j->partial_len++] = ch;
                }
                /* overlong line: silently truncate to LINEBUF_MAX */
            }
            /* loop to try draining more */
        } else if (r == 0) {
            /* EOF: flush any trailing partial line */
            if (j->partial_len > 0) {
                j->partial[j->partial_len] = '\0';
                job_ring_push(j, j->partial);
                j->partial_len = 0;
            }
            return 0;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ *
 * signal handlers
 * ------------------------------------------------------------------ */

static void on_sigchld(int sig) { (void)sig; got_sigchld = 1; }
static void on_term(int sig)    { (void)sig; stopping = 1; }

/* ------------------------------------------------------------------ *
 * reaping
 * ------------------------------------------------------------------ */

static void reap(void) {
    int status;
    struct rusage ru;
    pid_t pid;

    while ((pid = wait4(-1, &status, WNOHANG, &ru)) > 0) {
        job_t *j = job_find(pid);
        if (!j) continue;   /* not one of ours (shouldn't happen) */

        /* final drain of the capture pipe before we report */
        if (j->outfd >= 0) {
            while (drain_job(j) == 1) { /* keep going until EOF/again */ break; }
            /* do a blocking-ish final read loop: pipe write end is closed
             * in parent, child is dead, so EOF is imminent */
            int rc;
            do { rc = drain_job(j); } while (rc == 1);
        }

        struct timespec now; mono(&now);
        double wall = ts_diff(&j->start, &now);
        sum_wall += wall;

        long rss_kb = MAXRSS_IS_BYTES ? ru.ru_maxrss / 1024 : ru.ru_maxrss;
        if (rss_kb > peak_rss) peak_rss = rss_kb;

        int rc = WIFEXITED(status) ? WEXITSTATUS(status)
               : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;

        completed++;
        running--;

        if (rc == 0) {
            slog("done", "job %-*ld done   exit %-3d  %6.2fs  %8ldK",
                 ID_W, j->id, rc, wall, rss_kb);
        } else {
            failed++;
            fail_seen = 1;
            slog("fail", "job %-*ld FAILED exit %-3d  %6.2fs  %8ldK",
                 ID_W, j->id, rc, wall, rss_kb);
        }
        emit_output(j);
        job_free(j);
    }
}

/* ------------------------------------------------------------------ *
 * launching
 * ------------------------------------------------------------------ */

static void launch(void) {
    int pipefd[2] = {-1, -1};
    if (show_output && pipe(pipefd) != 0) {
        slog("info", "pipe() failed: %s", strerror(errno));
        /* proceed without capture for this job */
    }

    pid_t pid = fork();
    if (pid < 0) {
        slog("info", "fork() failed: %s", strerror(errno));
        if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
        return;
    }

    if (pid == 0) {
        /* child */
        if (show_output && pipefd[1] >= 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execvp(cmd[0], cmd);
        /* exec failed */
        _exit(127);
    }

    /* parent */
    job_t *j = job_alloc();
    j->pid = pid;
    j->id  = ++launched;
    mono(&j->start);
    running++;

    if (show_output && pipefd[0] >= 0) {
        close(pipefd[1]);               /* parent keeps read end only */
        int fl = fcntl(pipefd[0], F_GETFL, 0);
        fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);
        j->outfd = pipefd[0];
        job_ring_init(j);
    } else if (pipefd[0] >= 0) {
        close(pipefd[0]); close(pipefd[1]);
    }

    slog("run", "job %-*ld launched pid %-7d %ld running",
         ID_W, j->id, (int)pid, running);
}

/* ------------------------------------------------------------------ *
 * poll live capture pipes (drain output as it streams)
 * ------------------------------------------------------------------ */

static void poll_pipes(int timeout_ms) {
    if (!show_output) {
        if (timeout_ms > 0) {
            struct timespec t = { timeout_ms / 1000,
                                  (long)(timeout_ms % 1000) * 1000000L };
            nanosleep(&t, NULL);
        }
        return;
    }
    struct pollfd pfd[256];
    job_t *map[256];
    int nf = 0;
    for (long i = 0; i < jobs_cap && nf < 256; i++) {
        if (jobs[i].used && jobs[i].outfd >= 0) {
            pfd[nf].fd = jobs[i].outfd;
            pfd[nf].events = POLLIN;
            pfd[nf].revents = 0;
            map[nf] = &jobs[i];
            nf++;
        }
    }
    if (nf == 0) {
        if (timeout_ms > 0) {
            struct timespec t = { timeout_ms / 1000,
                                  (long)(timeout_ms % 1000) * 1000000L };
            nanosleep(&t, NULL);
        }
        return;
    }
    int r = poll(pfd, nf, timeout_ms);
    if (r <= 0) return;
    for (int i = 0; i < nf; i++) {
        if (pfd[i].revents & (POLLIN | POLLHUP)) {
            int rc = drain_job(map[i]);
            if (rc == 0) {           /* EOF: close, will be reaped soon */
                close(map[i]->outfd);
                map[i]->outfd = -1;
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * advance an absolute deadline by a fractional-second interval
 * ------------------------------------------------------------------ */

static void add_interval(struct timespec *t, double secs) {
    /* split without math.h: whole seconds + nanosecond remainder */
    long long ns_total = (long long)(secs * 1e9 + 0.5);
    t->tv_sec  += (time_t)(ns_total / 1000000000LL);
    t->tv_nsec += (long)(ns_total % 1000000000LL);
    if (t->tv_nsec >= 1000000000L) { t->tv_sec++; t->tv_nsec -= 1000000000L; }
}

/* ------------------------------------------------------------------ *
 * summary
 * ------------------------------------------------------------------ */

static struct timespec start_mono;

static void total(void) {
    reap();
    struct timespec now; mono(&now);
    double real = ts_diff(&start_mono, &now);
    double avg = completed > 0 ? sum_wall / (double)completed : 0.0;

    char ts[32]; timestamp(ts, sizeof ts);
    #define TLINE(fmt, ...) \
        printf("%s%s%s [%s%-*s%s] " fmt "\n", \
               C_DIM, ts, C_RESET, tag_color("total"), TAG_W, "total", C_RESET, __VA_ARGS__)

    putchar('\n');
    /* refresh ts per line so the column reflects real print time */
    timestamp(ts, sizeof ts); TLINE("launched   %ld", launched);
    timestamp(ts, sizeof ts); TLINE("completed  %ld", completed);
    timestamp(ts, sizeof ts);
    printf("%s%s%s [%s%-*s%s] failed     %s%ld%s\n",
           C_DIM, ts, C_RESET, tag_color("total"), TAG_W, "total", C_RESET,
           failed > 0 ? C_FAIL : "", failed, C_RESET);
    timestamp(ts, sizeof ts); TLINE("running    %ld", running);
    timestamp(ts, sizeof ts); TLINE("job-wall   %.3fs sum, %.3fs avg", sum_wall, avg);
    timestamp(ts, sizeof ts); TLINE("peak-rss   %ldK", peak_rss);
    timestamp(ts, sizeof ts); TLINE("real       %.3fs", real);
    #undef TLINE
}

/* ------------------------------------------------------------------ *
 * argument parsing -- long opts, short clustering (-eoL5)
 * ------------------------------------------------------------------ */

static void usage(FILE *f) {
    fprintf(f,
"Usage: %s [OPTIONS] -- COMMAND [ARGS...]\n"
"\n"
"  -i, --interval SEC    seconds between launches (default: 1.0)\n"
"  -n, --count N         total launches, 0 = unlimited (default: 0)\n"
"  -c, --concurrent N    cap on simultaneous jobs, 0 = unlimited (default: 0)\n"
"  -e, --stop-on-error   stop launching after the first job exits non-zero\n"
"  -o, --output          echo captured output of each finished job\n"
"  -L, --output-lines N  cap echoed lines per job, 0 = unlimited (default: 10)\n"
"      --color WHEN      colorize output: auto, always, never (default: auto)\n"
"  -q, --quiet           suppress per-launch lines, keep summary only\n"
"  -h, --help            this message\n"
"\n"
"Short flags may be combined: -eo, -eoq, -eoL5 (arg-taking flag must be last).\n"
"Exit status is non-zero if any job exited non-zero. Honors NO_COLOR.\n",
    prog);
}

static void die_opt(const char *msg, const char *arg) {
    fprintf(stderr, "%s: %s", prog, msg);
    if (arg) fprintf(stderr, " '%s'", arg);
    fputc('\n', stderr);
    exit(2);
}

/* set a named option; value may be NULL for flags. */
static void set_named(const char *name, const char *val) {
    if (!strcmp(name, "interval"))     { if (!val) die_opt("missing value for", name); interval = atof(val); }
    else if (!strcmp(name, "count"))      { if (!val) die_opt("missing value for", name); count = atol(val); }
    else if (!strcmp(name, "concurrent")) { if (!val) die_opt("missing value for", name); max_conc = atol(val); }
    else if (!strcmp(name, "output-lines")){ if (!val) die_opt("missing value for", name); output_lines = atol(val); }
    else if (!strcmp(name, "stop-on-error")) stop_on_err = 1;
    else if (!strcmp(name, "output"))     show_output = 1;
    else if (!strcmp(name, "quiet"))      quiet = 1;
    else die_opt("unrecognized option", name);
}

/* returns 1 if the short letter takes an argument */
static int short_takes_arg(char c, const char **name_out) {
    switch (c) {
        case 'i': *name_out = "interval";     return 1;
        case 'n': *name_out = "count";        return 1;
        case 'c': *name_out = "concurrent";   return 1;
        case 'L': *name_out = "output-lines"; return 1;
        case 'e': *name_out = "stop-on-error"; return 0;
        case 'o': *name_out = "output";       return 0;
        case 'q': *name_out = "quiet";        return 0;
        default:  *name_out = NULL;           return 0;
    }
}

static void parse_args(int argc, char **argv) {
    int i = 1;
    for (; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }

        if (a[0] == '-' && a[1] == '-') {
            /* long */
            char *name = a + 2;
            char *eq = strchr(name, '=');
            char namebuf[64];
            const char *val = NULL;
            if (eq) {
                size_t len = (size_t)(eq - name);
                if (len >= sizeof namebuf) die_opt("option too long", a);
                memcpy(namebuf, name, len); namebuf[len] = '\0';
                name = namebuf; val = eq + 1;
            }
            if (!strcmp(name, "help")) { usage(stdout); exit(0); }
            if (!strcmp(name, "color")) {
                const char *w = val;
                if (!w) { if (++i >= argc) die_opt("missing value for", "--color"); w = argv[i]; }
                if (!strcmp(w,"auto")) color_mode = COLOR_AUTO;
                else if (!strcmp(w,"always")) color_mode = COLOR_ALWAYS;
                else if (!strcmp(w,"never")) color_mode = COLOR_NEVER;
                else die_opt("invalid --color value", w);
                continue;
            }
            /* options that may take a value as next arg */
            if (!val && (!strcmp(name,"interval") || !strcmp(name,"count") ||
                         !strcmp(name,"concurrent") || !strcmp(name,"output-lines"))) {
                if (++i >= argc) die_opt("missing value for", name);
                val = argv[i];
            }
            set_named(name, val);
        }
        else if (a[0] == '-' && a[1] != '\0') {
            /* short cluster */
            const char *p = a + 1;
            while (*p) {
                char c = *p;
                if (c == 'h') { usage(stdout); exit(0); }
                const char *name;
                int takes = short_takes_arg(c, &name);
                if (!name) { char b[3]={'-',c,0}; die_opt("unrecognized option", b); }
                if (takes) {
                    /* value is rest of cluster, or next arg */
                    if (*(p+1)) { set_named(name, p+1); }
                    else {
                        if (++i >= argc) die_opt("missing value for", name);
                        set_named(name, argv[i]);
                    }
                    break;  /* arg-flag consumes remainder */
                } else {
                    set_named(name, NULL);
                    p++;
                }
            }
        }
        else {
            die_opt("expected '--' before COMMAND (got", a);
        }
    }

    if (i >= argc) { usage(stderr); exit(2); }
    cmd = &argv[i];
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    if (argc > 0 && argv[0][0]) {
        const char *base = strrchr(argv[0], '/');
        prog = base ? base + 1 : argv[0];
    }

    parse_args(argc, argv);
    color_setup();

    /* signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART;          /* we manage EINTR explicitly where needed */
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = on_term;
    sa.sa_flags = 0;                   /* allow interrupting sleeps */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    mono(&start_mono);

    slog("info", "starting: %s", cmd[0]);
    {
        char cbuf[32], mbuf[32];
        if (count == 0) snprintf(cbuf, sizeof cbuf, "inf"); else snprintf(cbuf, sizeof cbuf, "%ld", count);
        if (max_conc == 0) snprintf(mbuf, sizeof mbuf, "inf"); else snprintf(mbuf, sizeof mbuf, "%ld", max_conc);
        char obuf[32];
        if (show_output) snprintf(obuf, sizeof obuf, "on/%ldL", output_lines);
        else snprintf(obuf, sizeof obuf, "off");
        slog("info", "interval=%gs count=%s concurrent=%s stop-on-error=%s output=%s",
             interval, cbuf, mbuf, stop_on_err ? "on" : "off", obuf);
    }

    /* schedule: 'next' is the absolute deadline for the following launch.
     * It is seeded right after the first launch (see below) so the first
     * interval is measured from the actual start of job 1, not from here. */
    struct timespec next;
    int next_seeded = 0;

    while (!stopping) {
        if (count != 0 && launched >= count) break;

        if (got_sigchld) { got_sigchld = 0; reap(); }
        if (stop_on_err && fail_seen) break;

        /* concurrency cap: wait (draining pipes) until a slot frees */
        if (max_conc != 0) {
            while (running >= max_conc && !stopping) {
                poll_pipes(50);
                if (got_sigchld) { got_sigchld = 0; reap(); }
                if (stop_on_err && fail_seen) break;
            }
            if (stopping) break;
            if (stop_on_err && fail_seen) break;
        }

        launch();

        /* seed the schedule from the moment the first job actually started;
         * on every later launch, advance the absolute deadline by interval.
         * Anchoring to 'next' rather than 'now' is what keeps the period from
         * drifting: a slow iteration shortens the next wait instead of
         * pushing every subsequent launch back. */
        if (!next_seeded) { mono(&next); next_seeded = 1; }
        add_interval(&next, interval);
        for (;;) {
            if (stopping) break;
            if (got_sigchld) { got_sigchld = 0; reap(); }
            struct timespec now; mono(&now);
            double remain = ts_diff(&now, &next);
            if (remain <= 0) break;
            int ms = (int)(remain * 1000);
            if (ms <= 0) ms = 1;
            /* cap each wait slice so SIGCHLD reaping stays responsive */
            poll_pipes(ms > 50 ? 50 : ms);
        }
    }

    if (stop_on_err && fail_seen)
        slog("info", "stop-on-error: non-zero exit seen, no new launches");

    if (running > 0)
        slog("info", "waiting for %ld remaining job(s)...", running);
    while (running > 0) {
        poll_pipes(50);
        if (got_sigchld) { got_sigchld = 0; }
        reap();
        if (running > 0) {
            struct timespec t = {0, 50 * 1000000L};
            nanosleep(&t, NULL);
        }
    }

    total();

    /* cleanup */
    for (long k = 0; k < jobs_cap; k++)
        if (jobs[k].used) job_free(&jobs[k]);
    free(jobs);

    return fail_seen ? 1 : 0;
}
