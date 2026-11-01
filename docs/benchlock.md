# benchlock: one box, several sessions

`tools/benchlock/benchlock.py` is a machine-wide lock for the sessions that share a box: game runs whose timing
matters, builds, recompiles and correctness runs. Python 3.8+, standard library only; Linux and Windows. Put it on
`PATH` under a short name, for example:

```bash
ln -s "$PWD/tools/benchlock/benchlock.py" ~/.local/bin/benchlock
```

## bench or comp

| Mode | Holds the lock | Use it for |
| --- | --- | --- |
| `bench` | alone: nothing else runs | runs whose numbers matter: fps, profiles, A/B timings |
| `comp` | shared with other comps, never with a bench | builds, recompiles, test suites, runs that only check pictures or logs |

A comp never runs during a bench, so a measurement does not include someone's build. Several comps run at once
unless the box sets a limit:

```bash
benchlock config --comp-slots 2    # at most two comps at once on this box; 0 is unlimited
```

The limit is stored in the state directory, so every session on the box sees the same one.

## Leases

Every hold is a lease with a deadline. `-e` is your honest estimate in minutes, shown to everyone waiting; `-d` is the
hard deadline (default: twice the estimate, at least 5 minutes more), capped at 30 minutes for bench and 90 for comp.
Minutes may be fractions (`-e 0.5`).

```bash
benchlock run -t MYPORT -m comp -w "runtime build" -e 12 -- cmake --build build -j 16
```

`run` holds the lock while the command runs and kills the whole command (its process group on Linux, its Job object
on Windows) at the deadline; it exits with the command's status, 124 at the deadline, 75 when the wait for the lock
ran out (`--wait SECS`, default 30 minutes; `--wait 0` fails at once when busy).

When the work is not one command, `take` holds the lock from a background holder and prints the lease id:

```bash
id=$(benchlock take -t MYPORT -m bench -w "fps A/B, two builds" -e 15 | cut -d' ' -f1)
... runs ...
benchlock release --id "$id"
```

Release by `--id`. `release -t TAG` works only when that tag holds a single lease and is refused otherwise, since
several sessions may share a tag. A lease ends without any cleanup when its holder exits for any reason (finished,
killed, deadline, session gone): the lock is a kernel lock held by that process.

`benchlock status` lists the holders (tag, why, estimate, deadline) and the waiters in order. `benchlock wait` waits
until nothing holds the lock, without taking it.

## Waiting in order, asking to pass

Waiters take the lock in arrival order, so a waiting bench holds back the comps that arrive after it and builds
cannot starve a bench. A comp that arrives while only comps hold the lock and nobody waits goes straight in.

Arrival order is fair, but a two-minute check can sit behind a 45-minute build. A waiter can ask the waiters ahead of
it to let it pass, saying how long it needs and why; each of them decides for itself, and silence keeps the order:

```bash
benchlock status                                         # find your waiting id
benchlock ask --id MYPORT-1234 -e 2 -w "a fix the other ports wait for"
benchlock yield --id OTHER-5678 --to MYPORT-1234         # run by OTHER, ahead of MYPORT
```

`yield` moves the requester just ahead of the one who yields. A request lapses after 10 minutes; one open request per
waiter, and a tag asks at most once every 30 minutes. Once it has the lock, a waiter that asked holds it for the
minutes it asked with, at most. Good reasons to yield: a human waiting live beats unattended work; a short job beats a
long one when the long one loses little; a fix that blocks others beats routine measurement.

## Remote build boxes

`--host NAME` locks a remote build machine instead of this one; NAME is whatever you `ssh` to. It is a separate lock
kept on this machine, so the sessions on this box that share that remote host take turns on it. On a remote host comp
is exclusive too: one heavy job per box at a time.

```bash
benchlock run -t MYPORT -m comp --host buildbox -w "Windows cross build" -e 20 -- ssh buildbox 'cd src && ./build.sh'
```

## State and journal

The state directory is `$BENCHLOCK_STATE_DIR` if set, else `%LOCALAPPDATA%\ps2bench` on Windows, else
`$XDG_STATE_HOME/ps2bench` (`~/.local/state/ps2bench`). Every session that shares the lock must use the same one.

Every take, release, deadline expiry, dead holder, request to pass and yield is appended to `journal.log` there.
Nothing is posted anywhere by default. To relay the journal somewhere (a chat, a notifier), set `BENCHLOCK_ANNOUNCE`
to a command: it runs in the background with each journal line as its last argument.

## Windows

benchlock runs under native CPython: the python.org build or MSYS2's **UCRT64** (or CLANG64) `python`. There it uses
byte-range locks on the lock file, a detached holder for `take`, a Job object for `run`, and a marker file for
`release`.

Do not mix kinds of Python on one box: MSYS2's `msys` Python (`/usr/bin/python`, a Cygwin program) takes POSIX locks
that native Python does not see, and the reverse. Pick one, typically UCRT64's, and use it from every shell (MSYS2,
Git Bash, PowerShell). Under the msys Python the default state directory is also different (`~/.local/state` in the
MSYS2 home).

## Coming from tools/bench_lock.sh

| bench_lock.sh | benchlock |
| --- | --- |
| `bench_lock_take <port> <min> "<what>"` (one slot) | `benchlock take -t <port> -m comp -e <min> -w "<what>"` with `config --comp-slots 2` |
| `bench_lock_take ... alone` (both slots) | `benchlock take ... -m bench` |
| `bench_lock_release` | `benchlock release --id <id>`, or `benchlock run ... -- <cmd>` instead of take and release |
| `bench_lock_wait` | `benchlock wait` |
| `bench_lock_queue` | `benchlock status` |
| `bench_lock_ask_priority <port> <min> "<why>"` | `benchlock ask --id <waiting id> -e <min> -w "<why>"` |
| `bench_lock_yield <requester> <port>` | `benchlock yield --id <your waiting id> --to <requester's id>` |
| stale slot taken over past its "until" | the holder is killed at its deadline; a dead holder frees the lock at once |
| `PS2X_BENCH_LOCK`, `PS2X_BENCH_LOCK2`, `PS2X_BENCH_QUEUE` | `BENCHLOCK_STATE_DIR` |
| `PS2X_BENCH_LOCK_ANNOUNCE` | `BENCHLOCK_ANNOUNCE` |

## Tests

```bash
python3 tools/benchlock/benchlock.py --self-test
```

runs `tools/benchlock/test_benchlock.py`, each test against its own temporary state directory: bench exclusive and
comp shared, the comp slot limit, a remote host's lock, the deadline kill of `run` and of `take`, `release -t` refused
when a tag holds several leases, arrival order with a request to pass and a yield, and `wait`.
