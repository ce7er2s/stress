# stress

A small load-generator that repeatedly spawns a command in the background at a
fixed rate, with GNU-style aligned, colorized logging and per-job metrics.

It exists for time-tight stress testing: the logging path does zero forks per
line (no `date`, no `awk`), so emitting a log line costs microseconds rather
than the milliseconds a shell wrapper spends. Per-job wall time and peak RSS
come straight from `wait4`/`getrusage`, and launches are scheduled against
absolute deadlines so the interval does not drift under load.

## Build

```sh
make            # dynamic build (default)
make static     # fully static; uses musl-gcc if available, else glibc -static
make strip      # static + stripped, smallest artifact to ship
make debug      # -g + AddressSanitizer/UBSan, for development
make test       # build and run smoke tests
make install    # install to $(PREFIX)/bin   (default PREFIX=/usr/local)
```

No external dependencies, no `-lm`. Builds clean under `-Wall -Wextra`.

### Static builds

Static linking works on Linux and BSD. For a genuinely portable Linux binary,
build against musl (`musl-gcc`, packaged as `musl-tools` / `musl` in most
distributions); `make static` picks it up automatically. A glibc `-static`
build also works here because the program makes no NSS or locale calls.

macOS does not support static linking of libc — there the binary is always
dynamic against `libSystem`. That is a platform limitation, not a build issue.

## Usage

```
stress [OPTIONS] -- COMMAND [ARGS...]
```

| Option | | Meaning |
|---|---|---|
| `-i` | `--interval SEC` | Seconds between launches (default: 1.0) |
| `-n` | `--count N` | Total launches, 0 = unlimited (default: 0) |
| `-c` | `--concurrent N` | Cap on simultaneous jobs, 0 = unlimited (default: 0) |
| `-e` | `--stop-on-error` | Stop launching after the first job exits non-zero |
| `-o` | `--output` | Echo captured stdout/stderr of each finished job |
| `-L` | `--output-lines N` | Cap echoed lines per job, 0 = unlimited (default: 10) |
| | `--color WHEN` | `auto` (default), `always`, or `never` |
| `-q` | `--quiet` | Suppress per-launch lines, keep the summary |
| `-h` | `--help` | Help |

Everything after `--` is the command to run, taken literally.

Short flags may be combined: `-eo`, `-eoq`, `-eoL5`. A flag that takes an
argument must come last in the cluster, with its value attached (`-L5`) or as
the next word (`-eoL 5`).

The exit status is non-zero if any job exited non-zero — usable directly in CI.
`NO_COLOR` is honored, and color auto-disables when stdout is not a terminal.

## Examples

Hammer a health endpoint twice a second, capped at 10 in flight, forever:

```sh
stress -i 0.5 -c 10 -- curl -sf http://localhost:8080/health
```

Run a worker 100 times, stop at the first failure, show its output, fail CI:

```sh
stress -n 100 -eo -- ./bin/worker || echo "a run failed"
```

## Output

```
12:54:26.616 [info ] starting: ./bin/worker
12:54:26.616 [info ] interval=0.1s count=4 concurrent=2 stop-on-error=off output=on/2L
12:54:26.618 [run  ] job 1     launched pid 757     1 running
12:54:26.718 [run  ] job 2     launched pid 760     2 running
12:54:26.826 [done ] job 1     done   exit 0      0.21s      3448K
12:54:26.826 [out  ] job 1 output:
    | worker 757 starting
    | doing work line 2
    | ... (1 more lines)
...
12:54:27.xxx [total] launched   4
12:54:27.xxx [total] completed  4
12:54:27.xxx [total] failed     0
12:54:27.xxx [total] running    0
12:54:27.xxx [total] job-wall   0.83s sum, 0.21s avg
12:54:27.xxx [total] peak-rss   3460K
12:54:27.xxx [total] real       0.41s
```

Tags are padded inside the brackets so the `]` column stays fixed, which makes
the log easy to skim. Timestamps carry milliseconds for sub-second runs.
Summary fields: `job-wall` is the summed and averaged per-job wall time,
`peak-rss` the highest `ru_maxrss` observed, `real` the wall time of the whole
run.

## Behavior notes

**Interval scheduling is drift-compensated.** Deadlines are absolute
(`next += interval`), seeded from the actual start of the first job. A slow
iteration shortens the following wait instead of pushing every later launch
back, so the average rate holds even when per-iteration work varies. Measured
inter-launch spacing stays within about ±1 ms of the target.

**Reaping is event-driven.** A `SIGCHLD` handler sets a flag; the main loop
calls `wait4(WNOHANG)` in response. There is no busy polling of child state.

**`-o` ordering.** Captured output prints when a job finishes, i.e. in
completion order, not job-id order — the same as the `done`/`fail` lines. Under
concurrency, blocks from different jobs interleave as they complete. Only the
first N lines are kept (head semantics, like `head -n`).

**stop-on-error semantics.** "First error" means the first job that *finishes*
non-zero, not the first that will eventually fail. Under high concurrency a few
more jobs may already be in flight when the failure is detected; they are
allowed to drain. Running jobs are never killed. Use `-c 1` for strictly
sequential, deterministic stop-on-error.

## Known Limitations

- With `-o` and more than 256 simultaneously-live jobs, output beyond the first
  256 pipes isn't drained mid-flight in a given poll slice; it's flushed on
  final reap. Without `-o` there is no such cap. (Raising it means a dynamic
  `pollfd` array — straightforward if you need it.)
- `ru_maxrss` is reported in KB; the program normalizes the BSD/macOS
  bytes-vs-Linux-KB difference, but the value is still the OS's high-water mark
  for the whole child process, not a sampled curve.
